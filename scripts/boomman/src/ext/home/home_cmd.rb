
class GameHomeSystem < GameSystemBase

	class GameHomeCmd < ::Pack
		
		
	end
	
	HOME_TYPE_GET_HOME        = 0	# 获取家园信息	
	HOME_TYPE_UNLOCK_MAP      = 1	# 解锁地图
	HOME_TYPE_UPGRADE_MAP     = 2	# 升级地图
	HOME_TYPE_SET_MONSTER     = 3	# 设置守卫
	HOME_TYPE_MIX_MONSTER     = 4	# 合成守卫
	HOME_TYPE_GET_MAP_XML     = 5 # 下载地图XML
	HOME_TYPE_GAIN_TECH       = 6	# 学习科技
	HOME_TYPE_UPGRADE_TECH    = 7	# 升级科技
	HOME_TYPE_MIX_MONSTER_REQ = 8	# 合成守卫的需求
	HOME_TYPE_MATCH_HOME      = 9		# 匹配一个对手
	HOME_TYPE_SET_MAP         = 10		# 设置地图
	HOME_TYPE_ENTER_MAP       = 11		# 进入地图
	HOME_TYPE_BATTLE_RECORD		= 12 # 战斗记录

	
		
	def cmd_home(sender, pack)
		
		# 协议子类型
		type = pack.input.read_byte()
		
		case (type)
		when HOME_TYPE_GET_HOME
			handle_get_home(sender, pack.serial);
		
		when HOME_TYPE_UNLOCK_MAP
			map_id = pack.input.read_int16();
			handle_unlock_map(sender, map_id, pack.serial);
		
		when HOME_TYPE_UPGRADE_MAP
			serial = pack.input.read_int32();
			handle_upgrade(sender, serial, pack.serial);
			
		when HOME_TYPE_SET_MONSTER
			index = pack.input.read_byte
			monster_serial = pack.input.read_int32
			handle_set_monster(sender, index, monster_serial, pack.serial)
			
		when HOME_TYPE_MIX_MONSTER
			mid = pack.input.read_int32
			handle_mix_monster(sender, mid, pack.serial)
			
		when HOME_TYPE_GET_MAP_XML
			map_id = pack.input.read_int32();
			handle_download_xml(sender, map_id, pack.serial)
			
		when HOME_TYPE_GAIN_TECH
			tid = pack.input.read_int32();
			handle_gain_tech(sender, tid, pack.serial)
			
		when HOME_TYPE_UPGRADE_TECH
			tid = pack.input.read_int32();
			handle_upgrade_tech(sender, tid, pack.serial)
			
		when HOME_TYPE_MIX_MONSTER_REQ
			mid = pack.input.read_int32
			handle_mix_monster_req(sender, mid, pack.serial)
			
		when HOME_TYPE_MATCH_HOME
			
			handle_match(sender, pack.serial)
			
		when HOME_TYPE_SET_MAP
			serial = pack.input.read_uint32()
			gs  	 = pack.input.read_byte();
			guards = []
			for i in 0...gs
				guards << pack.input.read_uint32()
			end
		
			handle_set_map(sender, serial, guards, pack.serial)	
		
		when HOME_TYPE_ENTER_MAP
			pid = pack.input.read_uint32();
			hero_serial = pack.input.read_uint32();
			handle_enter_home(sender, pid, hero_serial, pack.serial);
		
		when HOME_TYPE_BATTLE_RECORD
			handle_battle_record(sender, pack.serial)
		
		end
		
		
		
	end
	
	
	def handle_match(sender, require_serial)
		
		player = sender.player
		home = player.home
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_MATCH_HOME)
		
		target_id = match(player.pid)
		# 匹配失败
		if(target_id != -1)
			simple_player = Player.create_simple_from_redis(target_id)
			target_home = load_home(target_id)
			target_map = target_home.find_home_map(target_home.cur_home_serial, :by_serial=>true)
			
			if(simple_player.nil? or target_home.nil? or target_map.nil?)
				os.write_byte(GameCMDS::FAIL)
			else
				os.write_byte(GameCMDS::SUCCESS)	

				os.write_uint32(player.home_battle_hero_serial)
				
				os.write_uint32(simple_player.pid)
				os.write_uint16(simple_player.level)
				os.write_byte(simple_player.sex)
				os.write_string(simple_player.name)
				os.write_string(simple_player.face)
				
				os.write_string(target_map.name)
				os.write_byte(target_map.map_level)
				os.write_byte(target_home.win_rate)
				
				os.write_int32(target_map.map_id)
				
			end
			
		else
			os.write_byte(GameCMDS::FAIL)
		end
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
		
	end
	
	def handle_get_home(sender, require_serial=0)
		
		player = sender.player
		home = player.home
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_GET_HOME)
		home.write_to_stream(os)
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
		
		
	end
	
	def handle_unlock_map(sender, map_id, require_serial=0)
		
		player = sender.player
		home = player.home
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_UNLOCK_MAP)
		
		map = home.gain_map(map_id)
		
		if(map != nil)
			os.write_byte(GameCMDS::SUCCESS)
			os.write_uint32(map.serial)
			os.write_uint16(map.id)
			sender.tips(Local.str("解锁#{map.name}成功"))
		else
			os.write_byte(GameCMDS::FAIL)	
			sender.tips(Local.str("解锁#{map.name}失败"))
		end
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
	
	end
	
	def handle_upgrade(sender, map_serial, require_serial=0)
		
		player = sender.player
		home = player.home
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_UPGRADE_MAP)
		
		map = home.find_home_map(map_serial, :by_serial=>true)
		
		# 检查前置条件
		if(map)
		
			require_level = map.upgrade_require_level;
			require_gold = map.upgrade_cost_gold;
			require_diamond = map.upgrade_cost_diamonds
			
			
			# 前置条件检查
			if(player.gold >= require_gold && 
				player.diamonds >= require_diamond && 
				player.level >= require_level)
			
				map = home.upgrade_map(map_serial);
				
			else
				map = nil
			end
			
		
		end
		
		if(map)
			os.write_byte(GameCMDS::SUCCESS)
			player.lose_gold(require_gold)
			player.lose_gold(require_diamond)
			os.write_uint32(map_serial)
			map.write_to_stream(os)

			sender.tips(Local.str("升级#{map.name}成功"))
		else
			os.write_byte(GameCMDS::FAIL)	
			sender.tips(Local.str("升级失败"))
		end
	
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
	
	
	end
	
	def handle_set_monster(sender, index, monster_serial, require_serial)
		
		player = sender.player
		home = player.home
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_SET_MONSTER)
		if(home.set_monster(monster_serial, index))
			os.write_byte(GameCMDS::SUCCESS)
			os.write_byte(index)
			os.write_int32(monster_serial)

			sender.tips(Local.str("设置守卫成功"))
		else
			os.write_byte(GameCMDS::FAIL)	
			sender.tips(Local.str("设置守卫失败"))
		end
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
	
	end
	
	
	def handle_download_xml(sender, map_id, require_serial)
		
		map = Map.find_map_by_id(map_id)
		
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_GET_MAP_XML)
		
		if(map.nil?)
			os.write_byte(0)
		else
			os.write_byte(1)
			os.write_int32(map_id);
			map.write_to_stream(os)
			
		end
		
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
		
		
	end
	
	
	def handle_gain_tech(sender, tid, require_serial)
		
		player = sender.player
		home = player.home
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_GAIN_TECH)
		ret_id = home.upgrade_tech(tid)

		if(ret_id == -1)
			os.write_byte(GameCMDS::FAIL)
			sender.tips(Local.str("购买科技失败"))
		else
			os.write_byte(GameCMDS::SUCCESS)
			os.write_int32(ret_id)
			sender.tips(Local.str("购买科技成功"))
		end
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
		
	end
	
	def handle_upgrade_tech(sender, tid, require_serial)
		
		player = sender.player
		home = player.home
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_UPGRADE_TECH)
		ret_id = home.upgrade_tech(tid)

		if(ret_id == -1)
			os.write_byte(GameCMDS::FAIL)
			sender.tips(Local.str("升级科技失败"))
		else
			tech = HomeTechnology.find_tech(ret_id)
			os.write_byte(GameCMDS::SUCCESS)
			tech.write_to_stream(os)
			sender.tips(Local.str("升级科技成功"))
		end
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
		

	end
	
	# 合成守卫的需求
	def handle_mix_monster_req(sender, mid, require_serial)
		
		player = sender.player
		home = player.home
		
		monster = HomeMonster.find_home_monster_by(mid)
		mix_item = HomeMonsterMix.mix_tables[mid]
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_MIX_MONSTER_REQ)
		
		if(mix_item.nil?)
			os.write_byte(GameCMDS::FAIL)
		else
			os.write_byte(GameCMDS::SUCCESS)		
			item1 = Item.find_templete(mix_item.req_fragment1_id)
			item2 = Item.find_templete(mix_item.req_fragment2_id)
			item3 = Item.find_templete(mix_item.req_fragment3_id)
			item4 = Item.find_templete(mix_item.req_fragment4_id)
			item5 = Item.find_templete(mix_item.req_fragment5_id)
			item6 = Item.find_templete(mix_item.req_item_id)
			req_gold = mix_item.gold_cost
			req_diamonds = mix_item.diamonds_cost
			
			items = [item1, item2, item3, item4, item5]
			if(item6 != nil)
				items << item6
			end
			
			os.write_byte(items.length)
			for i in items
				if(i.nil?)
					rails("mix monster info err #{mix_item}")
				end
				i.write_simple_to_stream(os)
			end
			os.write_uint32(req_gold)
			os.write_uint32(req_diamonds)
			os.write_string(monster.name)
			os.write_int32(monster.id)
		end
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
		
		
	end
	
	
	def handle_set_map(sender, serial, guards, require_serial)
		
		player = sender.player
		home = player.home
		
		ret = home.set_map(serial, guards)
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_SET_MAP)
		
		if(ret)
			os.write_byte(GameCMDS::SUCCESS)		
			os.write_uint32(serial)
			sender.tips(Local.str("设置地图成功"))
		else
			os.write_byte(GameCMDS::FAIL)		
			sender.tips(Local.str("设置地图失败"))
		end
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
		
		
	end

	# 进入对方家园
	def handle_enter_home(sender, pid, hero_serial, require_serial);
	
		
		home = load_home(pid)
		map = nil
		if(home.nil?)
			sender.tips(L("家园 : #{home_id}  不存在"), require_serial)
			return;
		end
		map = home.map;
		
		if(map.nil?)
			sender.tips(L("家园 : #{home_id}  未设置地图"), require_serial)
			$game.err(home)
			return
		end
		
		player = sender.player
		
	 	hero =  player.find_hero_by_serial(hero_serial)
		
		if hero.nil?
			
			sender.tips(L("设置出战的序号不正常 #{hero_serial} "), require_serial);
			
			return
		end
		
		if !hero.can_be_battle?
			sender.tips(L("该英雄今日已达到出战次数上线"), require_serial);
			return
		end
		
		
		hero.add_battle_count()
		
		
		player.home_battle_hero_serial = hero_serial
		
		player.pve_state = PVEState.new()
		player.pve_state.hero_serial = hero_serial;
		player.pve_state.make_home_data(home, 0)
		cmd = CMDEnterMap.create(player.pve_state, require_serial)
		sender.send_pack(cmd);
		
		# 测试数据!!!
		# 先直接赢
		simple_player = Player.create_simple_from_redis(pid)
		player.home.attack_win_times += 1
		player.home.attack_gain_morale += 10
		player.home.add_battle_record("你", simple_player.name)
		home.defence_fail_times += 1
		home.defence_lose_morale += 10
		home.add_battle_record(player.name, "你")
		
		# 保存一下
		home.cache()
		home.save()
		
	end


	# 合成
	def handle_mix_monster(sender, mid, require_serial)
		
		player = sender.player
		home = player.home
	
		mix_item = HomeMonsterMix.mix_tables[mid]
		
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_MIX_MONSTER)
		
		cond = false
		if(mix_item)
			# 检查条件
			cond = (player.gold >= mix_item.gold_cost and
				player.diamonds >= mix_item.diamonds_cost and
				player.item_count(mix_item.req_fragment1_id) >= mix_item.req_fragment1_count and
				player.item_count(mix_item.req_fragment2_id) >= mix_item.req_fragment2_count and
				player.item_count(mix_item.req_fragment3_id) >= mix_item.req_fragment3_count and
				player.item_count(mix_item.req_fragment4_id) >= mix_item.req_fragment4_count and
				player.item_count(mix_item.req_fragment5_id) >= mix_item.req_fragment5_count and
				player.item_count(mix_item.req_item_id) >= mix_item.req_item_count);
				
		end
		
		if(mix_item != nil && cond)
			os.write_byte(GameCMDS::SUCCESS)
			
			player.lose_item(mix_item.req_fragment1_id, mix_item.req_fragment1_count)
			player.lose_item(mix_item.req_fragment2_id, mix_item.req_fragment2_count)
			player.lose_item(mix_item.req_fragment3_id, mix_item.req_fragment3_count)
			player.lose_item(mix_item.req_fragment4_id, mix_item.req_fragment4_count)
			player.lose_item(mix_item.req_fragment5_id, mix_item.req_fragment5_count)
			player.lose_item(mix_item.req_item_id, mix_item.req_item_count)
			player.lose_diamonds(mix_item.diamonds_cost)
			player.lose_gold(mix_item.gold_cost)
			monster = home.gain_monster( mix_item.target_monster_id )
			
			monster.write_info_to_stream(os);
			

			sender.tips(Local.str("合成#{monster.name}成功"))
		else
			os.write_byte(GameCMDS::FAIL)	

			sender.tips(Local.str("合成失败"))
		end
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
	
	
	end
	
	def handle_battle_record(sender, require_serial)
		
		player = sender.player
		home = player.home
		os = FSOutputStream.new()
		os.write_byte(HOME_TYPE_BATTLE_RECORD)
		
		os.write_uint32(home.attack_win_times)
		os.write_uint32(home.attack_fail_times)
		os.write_uint32(home.attack_gain_morale)
		os.write_uint32(home.defence_win_times)
		os.write_uint32(home.defence_fail_times)
		os.write_uint32(home.defence_lose_morale)
		
		os.write_byte(home.battle_record.size)
		for i in 0...home.battle_record.size
			os.write_string(home.battle_record[i])
		end
		
		pack = Pack.create( require_serial, PACK_TYPE_HOME, os );
		sender.send_pack(pack);
	
		
	end
	
	
end
