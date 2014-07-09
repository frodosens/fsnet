require 'modules/basemode.rb'

class GameGemSystem < GameSystemBase


	#==========================================================================================
	#   寶石的效果
	# 	By Frodo	2014-07-06
	#==========================================================================================
	class GemEffect < BaseModule
	 
		attr_reader :id
		attr_reader :name
		attr_reader :maxhp_rate
		attr_reader :maxhp
		attr_reader :attack_rate
		attr_reader :attack
		attr_reader :move_speed_rate
		attr_reader :move_speed
		attr_reader :defence_rate
		attr_reader :defence
		attr_reader :boom_reload_rate
		attr_reader :boom_reload
		attr_reader :skill_cdtime_rate
		attr_reader :skill_cdtime
		attr_reader :boom_damage_weaken_rate
		attr_reader :boom_damage_weaken
		attr_reader :dot_delay
		attr_reader :dot_times
		attr_reader :dot_value
	
		def initialize
			super()
			@id = 0
			@name = ""
			@maxhp_rate = 0.0
			@maxhp = 0
			@attack_rate = 0.0
			@attack = 0
			@move_speed_rate = 0.0
			@move_speed = 0.0
			@defence_rate = 0.0
			@defence = 0
			@boom_reload_rate = 0.0
			@boom_reload = 0;
			@skill_cdtime_rate = 0.0
			@skill_cdtime = 0.0
			@boom_damage_weaken_rate = 0.0
			@boom_damage_weaken = 0.0
			@dot_delay = 0.0
			@dot_times = 0.0
			@dot_value = 0
		end
	
	
		def write_to_stream(os)
			os.write_float(@maxhp_rate)
			os.write_int32(@maxhp)
			os.write_float(@attack_rate)
			os.write_int32(@attack)
			os.write_float(@move_speed_rate)
			os.write_float(@move_speed)
			os.write_float(@boom_reload_rate)
			os.write_float(@boom_reload)
			os.write_float(@skill_cdtime_rate)
			os.write_float(@skill_cdtime)
			os.write_float(@boom_damage_weaken_rate)
			os.write_float(@boom_damage_weaken)
		end

		@@buffs = {}
		def self.reload_templete()
		
			@@buffs = {}
			CSV.load_csv("res/tmp_gems_effect.csv") do |hash|
				buff = new()
				buff.init_from_hash(hash)
				@@buffs[buff.id] = buff
			end
		
		end
	
		def self.find_effect(id)
			return @@buffs[id]
		end
	
	end


end