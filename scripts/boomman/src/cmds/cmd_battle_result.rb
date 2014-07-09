


require 'cmds/boomman_pack_type.rb'
class CMDBattleResult < Pack
	
	BATTLE_RESULT_WIN  = 0
	BATTLE_RESULT_FAIL = 1
	BATTLE_RESULT_ERR  = 2

	BATTLE_RESULT_ERR_CODE = 0	# pve状态异常
	
	
	class PVELog
		
		PVE_LOG_PLACE_BOMB    = 0		# 放下炸弹
		PVE_LOG_PLAYER_DAMAGE = 1		# 玩家受到伤害
		PVE_LOG_ENEMY_DAMAGE  = 2		# 怪物受到伤害
		PVE_LOG_PICK_MONEY    = 3		# 捡到金币 
		PVE_LOG_PICK_ITEM     = 4		# 捡到道具
		PVE_LOG_PICK_TIME     = 5		# 捡到时间
		PVE_LOG_KILL_ENEMY    = 6		# kill敌人
		PVE_LOG_PICK_BOX      = 7		# 捡到宝箱

		
		
		attr_reader :type
		
		# PVE_LOG_PLAYER_DAMAGE, PVE_LOG_ENEMY_DAMAGE
		attr_reader :damage
		attr_reader :damage_from
		
		# PVE_LOG_ENEMY_DAMAGE
		attr_reader :enemy_id
		
		# PVE_LOG_PICK_BOX, PVE_LOG_KILL_ENEMY
		attr_reader :level
		
		# PVE_LOG_PICK_MONEY
		attr_reader :mid
		
		# PVE_LOG_PLACE_BOMB, PVE_LOG_PICK_MONEY, PVE_LOG_PICK_ITEM
		attr_reader :x
		attr_reader :y
		
		# PVE_LOG_PICK_TIME
		attr_reader :time
		
		
		
		
		def init_from_is(is)
			
			self.type = is.read_byte
		
			case type
			when PVE_LOG_PLACE_BOMB    		# 放下炸弹
				pid = is.read_int32()
				@x = is.read_int16();
				@y = is.read_int16();
			when PVE_LOG_PLAYER_DAMAGE 		# 玩家受到伤害
				@damage_from = is.read_string();
				@damage 		 = is.read_int32()
			when PVE_LOG_ENEMY_DAMAGE  		# 怪物受到伤害
				@mid = is.read_int32()			# 哪只怪物
				@damage_from = is.read_string();
				@damage 		 = is.read_int32()
			when PVE_LOG_PICK_MONEY    		# 捡到金币 
				@mid = is.read_int32();
				@x = is.read_int32()
				@y = is.read_int32()
			when PVE_LOG_PICK_ITEM     		# 捡到道具
				@mid = is.read_int32();
				@x = is.read_int32()
				@y = is.read_int32()
			when PVE_LOG_PICK_TIME     		# 捡到时间
				@time = is.read_float()
			when PVE_LOG_KILL_ENEMY    		# kill敌人
				@level = is.read_short()
			when PVE_LOG_PICK_BOX      		# 捡到宝箱
				@level = is.read_short()
			end
			
		end
		
		class << self
			def create_from_is(is)
				log = new()
				log.init_from_is(is)
				return log
			end
		end
		
	end
	
	class << self
		
		def create_err(require_serial, code)
			
			os = FSOutputStream.new
			os.write_byte(BATTLE_RESULT_ERR)
			os.write_byte(code)
			
			pack = Pack.create( request_serial, PACK_TYPE_BATTLE_RESULT, os )
			return pack
		end
		
		def create_fail(require_serial)
			
			os = FSOutputStream.new
			os.write_byte(BATTLE_RESULT_FAIL)
			
			pack = Pack.create( request_serial, PACK_TYPE_BATTLE_RESULT, os )
			return pack
			
		end
		
		def create_win(require_serial, socre, exp, gold, pre_exp, pre_level, now_level)
			
			
			os = FSOutputStream.new
			
			os.write_byte(BATTLE_RESULT_WIN)
			os.write_byte(socre)
			os.write_int32(exp)
			os.write_int32(gold)
			os.write_int32(pre_exp)
			os.write_int16(pre_level)
			os.write_byte(now_level - pre_level)
			for i in pre_level...now_level
				Hero.hero_level_templete(i)
			end
			
			pack = Pack.create( request_serial, PACK_TYPE_BATTLE_RESULT, os )
			return pack
		end
		
		def check_battle_log(battle_time, battle_ext_hp, map, player, hero, logs)
			
			pve_state = player.pve_state
			enemy_hp  = battle_ext_hp + pve_state.monster_max_hp
			hero_hp   = hero.max_hp
			bomb_count = 0
			
			for log in logs
				
				case log.type
				when PVE_LOG_PLACE_BOMB    		# 放下炸弹
					bomb_count += 1
				when PVE_LOG_PLAYER_DAMAGE 		# 玩家受到伤害
					
					# 如果是收到炸彈傷害,扣去炸彈弱化率
					if(log.damage_from ==  "bomb")
						hero_hp -= (hero.attack * (1 - hero.boom_damage_weaken))
					end
					
				when PVE_LOG_ENEMY_DAMAGE  		# 怪物受到伤害
					
					# 从玩家的对战怪物中找到属性
					enemy = pve_state.find_monster_by_id(log.enemy_id)
					if(log.damage_from ==  "bomb" and enemy != nil)
						enemy_hp -= (hero.attack * (1 - enemy.defence))
					end
					
				when PVE_LOG_PICK_MONEY    		# 捡到金币 
					
				when PVE_LOG_PICK_ITEM     		# 捡到道具
					
				when PVE_LOG_PICK_TIME     		# 捡到时间
					
				when PVE_LOG_KILL_ENEMY    		# kill敌人
					
				when PVE_LOG_PICK_BOX      		# 捡到宝箱
					
				end
				
					
			end
			
			
		end
		
		def execute(require_serial, sender, server, battle_level, battle_time, battle_ext_hp, logs)
			
			player = sender.player
			
			pve_state = player.pve_state
			
			pack = nil
			
			while true
			
				if(pve_state.nil?)
					pack = create_err(require_serial, BATTLE_RESULT_ERR_CODE)
					break
				end
				
				hero  = player.find_hero_by_serial(pve_state.hero_serial)
				if(hero.nil?)
					pack = create_err(require_serial, BATTLE_RESULT_ERR_CODE)
					break
				end
			
			
				map   = Map.find_map_by_id(pve_state.mapid)
				if(map.nil?)
					pack = create_err(require_serial, BATTLE_RESULT_ERR_CODE)
					break
				end

				
				
				if(check_battle_log(battle_time, battle_ext_hp, map, player, hero, logs))
				
					
				
				else
				
					pack = create_fail(require_serial)
					
				end
				
				break;
			end
			
			
		
		
			player.pve_state = nil
		
			if(pack != nil)
				sender.send_pack(pack);
			end
			
		end
		
		
	end
	
	
	
	
end