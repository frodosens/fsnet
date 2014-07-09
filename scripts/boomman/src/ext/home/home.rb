# encoding: UTF-8
class GameHomeSystem < GameSystemBase

	class Home < BaseModule
	
		attr_reader :cur_home_serial  	# 地图
		attr_reader :maps								# 地图组
		attr_reader :monsters						# 我的怪物组
		attr_reader :guards						  # 放置的守卫[serial]
		attr_reader :rank								# 积分
		attr_reader :pid								# 家园主人
		attr_reader :match_history			# 战斗历史
		attr_reader :morale							# 士气
		attr_reader :techs							# 科技

		attr_reader :battle_record			# 战斗记录
		attr_accessor :attack_win_times		# 进攻次数
		attr_accessor :attack_fail_times	# 进攻次数
		attr_accessor :defence_win_times	# 防守次数
		attr_accessor :defence_fail_times	# 防守次数
		attr_accessor :attack_gain_morale				# 因为进攻得到的士气值
		attr_accessor :defence_lose_morale				# 因为防守失去的士气值
		
		
		def initialize(pid)
			super()
			@maps = []
			@rank = 0
			@cur_home_serial = 0
			@guards = []
			@monsters = []
			@match_history = {}
			@pid = pid
			@morale = 0
			@techs = []
			@battle_record = []
		end
		
		
		#==========================================================================================
		# => 战斗记录条数
		#==========================================================================================
		def battle_record
			@battle_record ||= []
			return @battle_record
		end
		
		#==========================================================================================
		# => 增加战斗记录
		#==========================================================================================
		def add_battle_record(attack_name, target_name)
			battle_record << "#{attack_name} 对 #{target_name} 进行了进攻"
			if(battle_record.size >= 6)
				battle_record.shift();
			end
		end
		
		
		#==========================================================================================
		# => 攻击得到的士气
		#==========================================================================================
		def attack_gain_morale
			@attack_gain_morale ||= 0
			return @attack_gain_morale
		end
		
		
		#==========================================================================================
		# => 防守失去的士气
		#==========================================================================================
		def defence_lose_morale
			@defence_lose_morale ||= 0
			return @defence_lose_morale
		end
		
		
		#==========================================================================================
		# => 攻击胜利次数
		#==========================================================================================
		def attack_win_times
			@attack_win_times ||= 0
			return @attack_win_times
		end
		
		#==========================================================================================
		# => 攻击失败次数
		#==========================================================================================
		def attack_fail_times
			@attack_fail_times ||= 0
			return @attack_fail_times
		end
		
		
		#==========================================================================================
		# => 防御胜利次数
		#==========================================================================================
		def defence_win_times
			@defence_win_times ||= 0
			return @defence_win_times
		end
		
		
		#==========================================================================================
		# => 防御失败次数
		#==========================================================================================
		def defence_fail_times
			@defence_fail_times ||= 0
			return @defence_fail_times
		end
	  
		
		#==========================================================================================
		# => 胜率
		#==========================================================================================
		def win_rate()
			return 100
		end
		
		#==========================================================================================
		# => 添加战斗记录
		#==========================================================================================
		def add_match_history(target_pid)
			
			if(target_pid > 0)
				@match_history[target_pid] ||= 0
			end
			
			for key, value in @match_history
				@match_history[key] += 1
			
				# 超过指定次数,才可重新匹配
				if(@match_history[key] >= $game_homes.match_interval)
					@match_history.delete(key)
				end
			
			end
			
			
		end
	
	
		#==========================================================================================
		# => 生成入库的sql
		#==========================================================================================
		def generate_save_sql()
			
			maps_data = SafeSql.conver(@maps.to_yaml)
			history_data = SafeSql.conver(@match_history.to_yaml)
			guards_data =  SafeSql.conver(@guards.to_yaml)
			monsters_data = SafeSql.conver(@monsters.to_yaml)
			
			return "replace into tb_homes(pid, rank, cur_home_serial, maps, monsters, guards, match_history) values( #{@pid}, #{@rank}, #{@cur_home_serial}, '#{maps_data}', '#{monsters_data}', '#{guards_data}', '#{history_data}' )"
			
		end
	
		
		#==========================================================================================
		# => 是否已经匹配过
		#==========================================================================================
		def matched(target_pid)
			return @match_history[target_pid] != nil
		end
	
		#==========================================================================================
		# => 获取已经匹配的组
		#==========================================================================================
		def matched_pids
			return @match_history.keys
		end
	
		#==========================================================================================
		# => 积分段
		#==========================================================================================
		def rank_section
			return $game_homes.get_rank_section(@rank)
		end
	
		#==========================================================================================
		# => 设置积分
		#==========================================================================================
		def rank=(value)
			@rank ||= 0
			$game_homes.update_rank( @pid, @rank ,value)
			@rank = value
			
		end
		
	
		#==========================================================================================
		# => 设置士气
		#==========================================================================================
		def morale=(v)
			@morale = v
		end
	
		#==========================================================================================
		# => 已使用的cost值
		#==========================================================================================
		def cost
			ret = 0;
			for monster_serial in @guards
				monster = find_monster(monster_serial, :by_serial=>true)
				if(monster)
					ret += monster.cost
				end
			end
			return ret
		end
		
		
		
		#==========================================================================================
		# => 获取可合成数量
		#==========================================================================================
		def get_mix_count(player, mix)
			
			mix_count = 0
			item1_count = player.item_count(mix.req_fragment1_id) / mix.req_fragment1_count
			item2_count = player.item_count(mix.req_fragment2_id) / mix.req_fragment2_count
			item3_count = player.item_count(mix.req_fragment3_id) / mix.req_fragment3_count
			item4_count = player.item_count(mix.req_fragment4_id) / mix.req_fragment4_count
			item5_count = player.item_count(mix.req_fragment5_id) / mix.req_fragment5_count
			item6_count = mix.req_item_id == 0 ? 1 : player.item_count(mix.req_item_id) / mix.req_item_count
			gold_cost = mix.gold_cost == 0 ? 1 : player.gold / mix.gold_cost
			diamonds_cost = mix.diamonds_cost == 0 ? 1 : player.diamonds / mix.diamonds_cost
			counts = [item1_count, item2_count, item3_count, item4_count, item5_count]
			
			counts << item6_count if(mix.req_item_count != 0)
			counts << gold_cost if(mix.gold_cost != 0)
			counts << diamonds_cost if(mix.diamonds_cost != 0)
			
			count = counts.min
			
			return count
		end
		
		#==========================================================================================
		# => 写入流
		#==========================================================================================
		def write_to_stream(os)
			player = $game_players.find_player_by_pid(@pid)
			maps = []
			# 先获取所有未解锁的1级地图
			for key, value in HomeMap.home_map_templetes
				if(value.map_level == 1)
					my_map = find_home_map(value.map_type, :by_type=>true)
					if(my_map)
						maps << my_map
					else
						maps << value
					end	
				end
			end
			
			# 当前地图序列号
			os.write_int32(@cur_home_serial)
			
			# 写入所有地图
			os.write_int16(maps.length)
			for m in maps
				if(m.class == HomeMap)
					# 已解锁的情况.这里是实例
					os.write_uint32(m.serial);
					m.templete.write_to_stream(os);
				else 
					# 未解锁的情况.这里是模板, 为了客户端统一处理,这里写一个0序列号
					os.write_uint32(0);
					m.write_to_stream(os);
				end
			end

			# 写入已拥有怪物数量
			all_monsters = HomeMonster.templete_table.keys
			os.write_int16(@monsters.length)
			for m in @monsters
				m.write_info_to_stream(os)
				mix = HomeMonsterMix.find_mix_by(m.id)
				all_monsters.delete(m.id)
			end
			
			# 写入未拥有数量
			os.write_int16(all_monsters.length)
			for mid in all_monsters
				m = HomeMonster.find_home_monster_by(mid)
				m.write_to_stream(os)
				mix = HomeMonsterMix.find_mix_by(m.id)
				
			end
			
			
			# 写入已上阵的怪物序列号
			os.write_int16(@guards.length)
			for g in @guards
				os.write_int32(g)
			end
			
			techs = []
			templetes = HomeTechnology.techs_templete;
			for key  in templetes.keys.sort
				if(templetes[key].level == 1)
					my_tech = find_tech(templetes[key].type, :by_type=>true)
					# 未已学习
					if(my_tech.nil?)
						techs << templetes[key];
					end
				end
			end
			
			# 写入所有科技
			os.write_int16(@techs.length + techs.length)
			# 写入已学习的科技
			for tid in @techs
				tech = HomeTechnology.find_tech(tid)
				os.write_byte(1)
				tech.write_to_stream(os)
			end
			
			
			# 写入未学习的科技
			for tech in techs
				os.write_byte(0)
				tech.write_to_stream(os)
			end
			
			# 写入士气
			os.write_uint16(@morale)
			
			
			
			
			maps = nil
			
		end
		
		#==========================================================================================
		# => 设置守卫
		#==========================================================================================
		def set_monster(monster_serial, index)
			return false if(index < 0 || index >= @guards.length) 
			
			monster = find_monster(monster_serial, :by_serial=>true)
			
			cur_cost = self.cost
			if(monster)
				# 这个位置已经设置过了
				if(@guards[index] != 0)
					org_monster = find_monster(@guards[index], :by_serial=>true)
					if(org_monster)
						
						# 减去旧的,然后再加上新的 如果不够cost的话.
						if( ((cur_cost - org_monster.cost) + monster.cost) > map.max_cost)
							return false
						end
						
						# 先吧旧的卸下
						org_monster.undeploymente();
					end
				end
				# 新的上阵
				if(monster.deploymente())
					@guards[index] = monster_serial;
					return true
				end
			end
			return false
		end
	
		
		#==========================================================================================
		# => 清空守卫
		#==========================================================================================
		def clear_guards()
			for i in 0...@guards.length
				if(@guards[i] != 0)
					monster = find_monster(@guards[i], :by_serial=>true)
					monster.undeploymente();
				end
				@guards[i] = 0
			end
			
			
		end
		
		#==========================================================================================
		# => 设置新的地图
		#==========================================================================================
		def set_map(serial, guards)
			
			my_map = find_home_map(serial, :by_serial=>true)
			if(my_map)
				
				old_serial = @cur_home_serial
				old_guards = @guards.clone
				
				@cur_home_serial = serial
			
				# 卸下旧的
				clear_guards();
				
				@guards = Array.new(my_map.monster_count, 0)
				
				set_success = false;
				
				# 设置守卫
				if(guards != nil)
					for i in 0...guards.size
						set_success |= set_monster(guards[i], i);
					end
					
					# 如果所有设置都不成功, 还原属性
					if(!set_success)
						@cur_home_serial = old_serial
						clear_guards();
						for i in 0...old_guards.size
							set_monster(old_guards[i], i);
						end
						return false;
					end
				
					
				end
				
				
				return true
			end
			return false
		end
		
		#==========================================================================================
		# => 家园地图
		#==========================================================================================
		def map
			return find_home_map(@cur_home_serial, :by_serial=>true)
		end
		
		#==========================================================================================
		# => 通过key找到我的地图
		#==========================================================================================
		def find_home_map(key, oper={ :by_id=>true, :by_type=>false, :by_serial=>false })
			for map in @maps
				cond = true
				if(oper[:by_id])
					cond &= map.id == key
				end
				if(oper[:by_type])
					cond &= map.map_type == key
				end
				if(oper[:by_serial])
					cond &= map.serial == key
				end
				if(cond)
					return map;
				end
			end
			return nil
		end
		
		#==========================================================================================
		# => 通过key找到我的守卫
		#==========================================================================================
		def find_monster(key, oper={ :by_id=>true, :by_serial=>false })
			for monster in @monsters
				cond = true
				if(oper[:by_id])
					cond &= monster.id == key
				end
				if(oper[:by_serial])
					cond &= monster.serial == key
				end
				if(cond)
					return monster;
				end
			end
			return nil
		end
		
		
		#==========================================================================================
		# => 通过key找到科技
		#==========================================================================================
		def find_tech(key, oper={ :by_id=>true, :by_type=>false, :by_level=>nil, :by_mutil=>false })
			
			@techs ||= []
			
			ret = nil
			for tid in @techs
				t = HomeTechnology.find_tech(tid);
				cond = true
				
				if(oper[:by_id])
					cond &= t.id == key
				end
				if(oper[:by_type])
					cond &= (t.type == key)
				end
				if(oper[:by_level] != nil)
					cond &= (t.level == oper[:by_level])
				end
				
				if(cond)
					
					if( oper[:by_mutil] )
						ret ||= []
						ret << t
					else
						return t		
					end
					
				end
				
			end
			
			return ret
		end
		
		#==========================================================================================
		# => 升级一个科技
		# 如果没有该类型的科技,则为学习
		# 如果已有同类型的科技,则为升级
		#==========================================================================================
		def upgrade_tech(tid)
			
			tech = HomeTechnology.find_tech(tid);
			return -1 if(tech.nil?);
			my_tech = find_tech(tech.type, :by_type=>true)
			# 如果已经有此类型科技
			if(my_tech)
				# 该类型的下一级科技
				next_level_tech = HomeTechnology.find_tech(tech.type, :by_type=>true, :by_level=>my_tech.level + 1)
				if(next_level_tech != nil)
					# 删除旧科技
					@techs.delete(my_tech.id)
					# 得到新科技
					@techs << next_level_tech.id
					return next_level_tech.id
				else
					return -1
				end
				
			else
				@techs << tid
				return tid
			end
			
		end
		
		
		#==========================================================================================
		# => 得到一个monster
		#==========================================================================================
		def gain_monster(m_id)
			
			my_monster = find_monster(m_id)
			
			if(my_monster != nil)
				my_monster.gain()
				return my_monster
			end
			
			monster = HomeMonster.create_from_templete_id(m_id)
			return nil if(monster.nil?)

			@monsters << monster;
			
			return monster
			
		end
		
		#==========================================================================================
		# => 得到一个地图
		#==========================================================================================
		def gain_map(map_id)
			# 检查地图合法性
			map = HomeMap.find_map_by_id(map_id)
			
			return nil if(map.nil?)
			# 如果已有同类型的地图了
			my_map = find_home_map(map.map_type, :by_type=>true)
			
			return nil if(!my_map.nil?)
		
			map = HomeMap.create_from_templete_id(map_id);
			return nil if(map.nil?)
		
			@maps << map;
		
			return map
		end
		
		#==========================================================================================
		# => 升级地图
		#==========================================================================================
		def upgrade_map(serial)

			my_map = find_home_map(serial, :by_serial=>true)
			
			return false if(my_map.nil?)
			
			
			if(my_map.upgrade())
				return my_map;
			else
				return nil;
			end
			
		end
		
		#==========================================================================================
		# => 保存到database
		#==========================================================================================
		def save

			sqls = []
			sqls <<  generate_save_sql();
			$game_database.try_remoot_execute(sqls);
			
		end
		
		#==========================================================================================
		# => 获取这个家园的缓存cache
		#==========================================================================================
		def cache_key
			return Home.generate_cache_key(@pid)
		end
		
		class << self
			
			#==========================================================================================
			# => 根据PID生成缓存KEY
			#==========================================================================================
			def generate_cache_key(pid)
				return "game_homes:homes:#{pid}";		
			end
			

			#==========================================================================================
			# => 通过db创建一个player
			#==========================================================================================
			def create_from_database(pid)
			
				sql = "select * from tb_homes where pid=#{pid} limit 0,1"
				result = $game_database.query(sql);
				if(result and result.size > 0)
					home = new(0);
					result.each do |row|
						home.init_from_hash(row)
					end
					return home
				end
			
				return nil
			end
				
				
		end
		
	end
end