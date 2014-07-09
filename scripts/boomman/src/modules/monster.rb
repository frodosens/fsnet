
require 'modules/basemode.rb'
#==========================================================================================
#   怪物模板
# 	By Frodo	2014-06-13
#==========================================================================================
class Monster < BaseModule
	
	attr_reader :id
	attr_reader :name
	attr_reader :type
	attr_reader :level
	attr_reader :starlevel
	attr_reader :qualitylevel
	attr_reader :model_id_list
	attr_reader :hp_limit
	attr_reader :attack
	attr_reader :defence
	attr_reader :movespeed
	attr_reader :ai_script
	
	alias :max_hp :hp_limit
	def initialize()
		super()
		@id = 0
		@name = ""
		@type = 0
		@level = 0
		@starlevel = 1
		@qualitylevel = 1
		@model_id_list = []
		@hp_limit = 0
		@attack = 0
		@defence = 0
		@movespeed = 0
		@ai_script = ""
		@size_width = 1
		@size_height = 1
	end
	
	#==========================================================================================
	# => 获取星级模板
	#==========================================================================================
	def tmp_star
		return Hero.star_templete[@starlevel];
	end
	#==========================================================================================
	# => 获取品质模板
	#==========================================================================================
	def tmp_quality
		return Hero.quality_templete[@qualitylevel];
	end
	
	
	def max_hp
		return @hp_limit * (1 + tmp_star.value) * (1 + tmp_quality.value)
	end
	def attack
		return @attack * (1 + tmp_star.value) * (1 + tmp_quality.value)
	end
	def defence
		return @defence * (1 + tmp_star.value) * (1 + tmp_quality.value)
	end
	def movespeed
		return @movespeed
	end
	
	def model
		return @model_id_list[0]
	end
	
	def write_to_stream(os)
		
		os.write_int32(id)
		os.write_string(self.name)
		os.write_int32(self.max_hp)
		os.write_float(movespeed.to_f)
		os.write_byte(@size_width)
		os.write_byte(@size_height)
		os.write_int32(self.attack)
		os.write_float(self.defence.to_f)
		os.write_string(@ai_script)
		os.write_uint32(model)
		
	end
	
	
	
	class << self
		@@pve_monster = {}
		def reload_templete
			@@pve_monster = {}
			CSV.load_csv("res/tmp_monster_pve.csv") do |hash|
				monster = new()
				@@pve_monster[hash["id"].to_i] = monster.init_from_hash(hash);
			end
		end
		
		def find_pve_monster_by(mid)
			return @@pve_monster[mid];
		end
		
		
	end
	
	
end