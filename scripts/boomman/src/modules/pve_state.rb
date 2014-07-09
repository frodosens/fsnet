#==========================================================================================
#   玩家PVE的临时状态
# 	By Frodo	2014-06-10
#==========================================================================================
class PVEState

	attr_reader :mapid	  		# 挑战中的关卡等级
	attr_reader :golds				# 该地图会掉落的金币
	attr_reader :items				# 该地图会掉落的道具
	attr_accessor :hero_serial	# 出战Hero
	attr_reader :monsters			# 战斗的怪物
	attr_reader :monster_max_hp	# 战斗的怪物
	def initialize()
		@mapid = 0
		@golds = []
		@items = []
		@monsters = []
		@monster_max_hp = 0
		@hero_serial = 0
	end
	
	def write_to_stream(os)



		map = Map.find_map_by_id(@mapid)
		map.write_to_stream(os)
		os.write_uint16(@monsters.length)
		for m in @monsters
			m.write_to_stream(os);
		end
		
		os.write_uint32(hero_serial)
		
	end
	
	def make_pve_data(map_id)
		@mapid = map_id
		@monster_max_hp = 0
		map = Map.find_map_by_id(map_id)
		
		for obj in map.objects
			if(obj.monster? && obj.monster != nil)
				@monster_max_hp += obj.monster.max_hp
				@monsters << obj.monster.deep_clone
			end
		end
	end
	
	def make_home_data(home, battle_point)
		
		@mapid = home.map.map_id
		@monster_max_hp = 0
		@monsters = []
		
		for g in home.guards
			
			monster = home.find_monster(g, :by_serial=>true)
			if(monster != nil)
				@monsters << monster.to_pve_monster(battle_point)
			end
		end
		
		
	end


	def find_monster_by_id(id)
		for m in @monsters
			if(m.id == id)
				return m
			end
		end
	end

end
