

require 'modules/basemode.rb'
class AttributeTemplete < BaseModule
	
	attr_reader :base_movespeed
	attr_reader :base_boomcount
	attr_reader :base_boomrange
	attr_reader :base_boomreload
	attr_reader :battle_point_v_a
	attr_reader :battle_point_v_b
	attr_reader :battle_point_v_c
	attr_reader :battle_point_v_d
	def initialize
		@base_movespeed = 0.0
		@base_boomcount = 0
		@base_boomrange = 0
		@base_boomreload = 0
		@battle_point_v_a = 0
		@battle_point_v_b = 0
		@battle_point_v_c = 0
		@battle_point_v_c = 0
	end
	
	
	class << self
		@@attrib_templete = nil
		
		def templete
			return @@attrib_templete
		end
		
		def reload_templete
			
			@@attrib_templete = AttributeTemplete.new

			CSV.load_csv("res/tmp_attribute_base.csv") do |hash|
				@@attrib_templete.init_from_hash(hash)
			end
			return @@attrib_templete
			
		end
		
	end
	
end