

require 'modules/basemode.rb'

class GameGemSystem < GameSystemBase
	

	#==========================================================================================
	#   宝石合成项
	# 	By Frodo	2014-06-13
	#==========================================================================================
	class GemMix < BaseModule
	
		attr_reader :org_item_id
		attr_reader :org_item_count
		attr_reader :target_item_id
		attr_reader :req_gold
		attr_reader :req_diamonds
	
		def initialize
			@org_item_id = 0
			@org_item_count = 0
			@target_item_id = 0
			@req_gold = 0
			@req_diamonds = 0
		end

		@@mixs = {}
		def self.reload_templete()
		
			@@mixs = {}
			CSV.load_csv("res/tmp_gems_mix.csv") do |hash|
				mix = new()
				mix.init_from_hash(hash)
				@@mixs[mix.org_item_id] = mix
			end
		
		end
		
		def self.find_mix(item_id)
			return @@mixs[item_id]
		end
	
	end
	
end