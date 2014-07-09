
class GameHomeSystem < GameSystemBase

	# 家园地图模板
	class HomeMapTemplete < ::BaseModule
		
		attr_reader :id
		attr_reader :name
		attr_reader :map_type
		attr_reader :map_level
		attr_reader :map_star
		attr_reader :map_id
		attr_reader :upgrade_require_level
		attr_reader :upgrade_cost_gold
		attr_reader :upgrade_cost_diamonds
		attr_reader :monster_count
		attr_reader :max_cost
		attr_reader :trap_count
		attr_reader :organ_count
		attr_reader :max_level
		
		def initialize
			@id = 0
			@name = ""
			@map_type = 0
			@map_level = 0
			@map_star = 0
			@map_id = 0
			@upgrade_require_level = 0
			@upgrade_cost_gold = 0
			@upgrade_cost_diamonds = 0
			@monster_count = 0
			@max_cost = 0
			@trap_count = 0
			@organ_count = 0
			@max_level = 0
		end
	
		def write_to_stream(os)
			os.write_int16(@id)
			os.write_string(@name)
			os.write_int16(@map_type)
			os.write_int16(@map_level)
			os.write_byte(@map_star)
			os.write_int32(@map_id)
			os.write_uint16(@upgrade_require_level)
			os.write_uint32(@upgrade_cost_gold)
			os.write_uint32(@upgrade_cost_diamonds)
			os.write_byte(@monster_count)
			os.write_uint16(@max_cost)
			os.write_byte(@trap_count)
			os.write_byte(@organ_count)
			os.write_byte(@max_level)
		end
	
	end

	# 家园地图实例
	class HomeMap < ::BaseModule
	
		def upgrade
			next_level_map = HomeMap.find_map_next_level(@templete_id)
			if(next_level_map.nil?)
				return false
			else
				# 直接换模板ID
				@templete_id = next_level_map.id
				return true;
			end
		end
	
		def templete
			return @@home_map_templete[@templete_id]
		end
	
	
		class << self
			
			@@home_map_templete = {}
			def reload_templete
				@@home_map_templete = {}
				CSV.load_csv("res/tmp_terrain.csv") do |hash|
					map = HomeMapTemplete.new()
					@@home_map_templete[hash["id"].to_i] = map.init_from_hash(hash);
				end
			end
			
			def home_map_templetes
				return @@home_map_templete
			end
	
			def find_map_by_id(map_id)
				return @@home_map_templete[map_id];
			end
	
			def find_map_next_level(map_id)
				map = @@home_map_templete[map_id];
				for key,value in @@home_map_templete
					if((value.map_type == map.map_type) and (value.map_level == map.map_level + 1))
						return value
					end
				end
				return nil
			end
			
			
			
		end
	end



end