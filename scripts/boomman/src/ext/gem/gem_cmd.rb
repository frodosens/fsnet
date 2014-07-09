
class GameGemSystem < GameSystemBase
	
	GEMS_TYPE_MIX     = 0 # 合成宝石
	GEMS_TYPE_EQUIP   = 1 # 装备宝石
	GEMS_TYPE_TARGET  = 2 # 查看目标道具
	GEMS_TYPE_INFO    = 3 # 獲取基本信息
	GEMS_TYPE_MIX_ALL = 4 # 一鍵合成
	

	def cmd_gems(sender, pack)
		type = pack.input.read_byte()
		
		case type
			
		when GEMS_TYPE_MIX
			item_id = pack.input.read_uint32()
			handle_mix(sender, item_id, pack.serial)
		when GEMS_TYPE_EQUIP
			index  = pack.input.read_byte()
			gem_id = pack.input.read_uint32()
			handle_equip(sender, index, gem_id, pack.serial)
		when GEMS_TYPE_TARGET
			item_id = pack.input.read_uint32()
			handle_target(sender, item_id, pack.serial)
		when GEMS_TYPE_MIX_ALL
			handle_mix_all(sender, pack.serial)
		when GEMS_TYPE_INFO
			handle_info(sender, pack.serial)
		end	
		
	end
	
	# 合成
	def handle_mix(sender, item_id, serial)
		
		player = sender.player
		
		os = FSOutputStream.new()
		os.write_byte(GEMS_TYPE_MIX)
		
		
		mix = GemMix.find_mix(item_id)
		if(mix.nil?)
			os.write_byte(GameCMDS::FAIL)
		else
			
			success = (player.gold >= mix.req_gold) and (player.diamonds >= mix.req_diamonds)
			success &= player.item_count(mix.org_item_id) >= mix.org_item_count
			
			if(success)
				os.write_byte(GameCMDS::SUCCESS)

				player.lose_item(mix.org_item_id, mix.org_item_count)
				player.lose_gold(mix.req_gold)
				player.lose_diamonds(mix.req_diamonds)
				# 1个
				player.gain_item(mix.target_item_id, 1)
				
			else
				os.write_byte(GameCMDS::FAIL)	
			end
			
			
		end
			
		
		pack = Pack.create( serial, PACK_TYPE_GEMS, os );
		sender.send_pack(pack);
		
	end
	
	# 裝備
	def handle_equip(sender, index, gem_id, serial)
		
		player = sender.player
		
		os = FSOutputStream.new()
		os.write_byte(GEMS_TYPE_EQUIP)
		
		check = player.item_count(gem_id) > 0 || (gem_id == 0)
		
		
		success = false
		if(check > 0)
			success = player.set_gem(index, gem_id)
		else
			success = false
		end
		
		os.write_byte(success ? GameCMDS::SUCCESS : GameCMDS::FAIL)
		
		pack = Pack.create( serial, PACK_TYPE_GEMS, os );
		sender.send_pack(pack);
	end
	
	# 獲取合成目標信息
	def handle_target(sender, item_id, serial)
		
		os = FSOutputStream.new()
		os.write_byte(GEMS_TYPE_TARGET)
		
		mix = GemMix.find_mix(item_id)
		if(mix.nil?)
			os.write_byte(GameCMDS::FAIL)
		else
			
			os.write_byte(GameCMDS::SUCCESS)
			target_item = Item.find_templete(mix.target_item_id)		
			item1 = Item.find_templete(mix.org_item_id)		
			item2 = Item.find_templete(mix.org_item_id)		
			item3 = Item.find_templete(mix.org_item_id)		
			
			target_item.write_simple_to_stream(os);
			item1.write_simple_to_stream(os);
			item2.write_simple_to_stream(os);
			item3.write_simple_to_stream(os);
		end
		
		pack = Pack.create( serial, PACK_TYPE_GEMS, os );
		sender.send_pack(pack);
	end
	
	# 一鍵合成
	def handle_mix_all(sender, serial)
		
		
		player = sender.player
		gem_items = []
		# 先得到所有的道具ID
		for item in player.items
			if(item.type == ITEM_TYPE_GEMS)
				if(!gem_items.include?(item.id))
					gem_items << item.id
				end
			end
		end
		
		
		success = false
		ids = gem_items.sort
		while(ids.size > 0)
			
			id = ids.shift!
			
			mix = GemMix.find_mix(id)
			can_be_mix_count = gem_items[id] / mix.org_item_count
			
			if(can_be_mix_count > 0)
				# 先得到最大數量
				player.gain_item(mix.target_item_id, can_be_mix_count)
				player.lose_item(id, can_be_mix_count * mix.org_item_count)
				
				# 新的參與下一次統計
				ids << mix.target_item_id
				success = true
			end
			
			ids.delete(id)
			
		end
		os = FSOutputStream.new()
		os.write_byte(GEMS_TYPE_MIX_ALL)
		os.write_byte(success ? GameCMDS::SUCCESS : GameCMDS::FAIL)
		pack = Pack.create( serial, PACK_TYPE_GEMS, os );
		sender.send_pack(pack);
		
		
	end
	
	
	def handle_info(sender, serial)
		player = sender.player
		
		os = FSOutputStream.new()
		os.write_byte(GEMS_TYPE_INFO)

		holes = player.gems.gems_holes	
		
		os.write_byte(holes.size)
		
		for id in holes
			
			if(id != 0)
			
				item = Item.find_templete(id)
				effect = GemEffect.find_effect(item.tmp_gems_effect)
				
				os.write_byte(1)
				item.write_simple_to_stream(os)
				effect.write_to_stream(os)
			
			else
				os.write_byte(0)
			
			end
			
		end
		
		pack = Pack.create( serial, PACK_TYPE_GEMS, os );
		sender.send_pack(pack);
		
	end
	
end