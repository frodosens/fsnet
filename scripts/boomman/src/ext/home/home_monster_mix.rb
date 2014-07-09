
class GameHomeSystem < GameSystemBase
	
	
	class HomeMonsterMix < BaseModule
		
		attr_reader :target_monster_id
		attr_reader :req_fragment1_id
		attr_reader :req_fragment1_count
		attr_reader :req_fragment2_id
		attr_reader :req_fragment2_count
		attr_reader :req_fragment3_id
		attr_reader :req_fragment3_count
		attr_reader :req_fragment4_id
		attr_reader :req_fragment4_count
		attr_reader :req_fragment5_id
		attr_reader :req_fragment5_count
		attr_reader :req_item_id
		attr_reader :req_item_count
		attr_reader :gold_cost
		attr_reader :diamonds_cost
		
		def initialize
			super
			@target_monster_id = 0
			@req_fragment1_id = 0
			@req_fragment1_count = 0
			@req_fragment2_id = 0
			@req_fragment2_count = 0
			@req_fragment3_id = 0
			@req_fragment3_count = 0
			@req_fragment4_id = 0
			@req_fragment4_count = 0
			@req_fragment5_id = 0
			@req_fragment5_count = 0
			@req_item_id = 0
			@req_item_count = 0
			@gold_cost = 0
			@diamonds_cost = 0
		end
		
		def write_to_stream(os)
			os.write_int32(@req_fragment1_id)
			os.write_uint16(@req_fragment1_count)
			os.write_int32(@req_fragment2_id)
			os.write_uint16(@req_fragment2_count)
			os.write_int32(@req_fragment3_id)
			os.write_uint16(@req_fragment3_count)
			os.write_int32(@req_fragment4_id)
			os.write_uint16(@req_fragment4_count)
			os.write_int32(@req_fragment5_id)
			os.write_uint16(@req_fragment5_count)
			os.write_int32(@req_item_id)
			os.write_uint16(@req_item_count)
			os.write_uint32(@gold_cost)
			os.write_uint32(@diamonds_cost)
			
		end
		
		class << self
			@@home_monster_mix = {}
			
			def reload_templete
				@@home_monster_mix = {}
				CSV.load_csv("res/home_monster_mix.csv", :first_type=>true) do |hash|
					mix = HomeMonsterMix.new()
				  mix.init_from_hash(hash);
					@@home_monster_mix[mix.target_monster_id] = mix
				end
				
			end
			def find_mix_by(mid)
				return @@home_monster_mix[mid];
			end
			
			def mix_tables
				return @@home_monster_mix
			end
			
		end
		
		
	end
	
	
end