
class GameHomeSystem < GameSystemBase

	class HomeTechnology < ::BaseModule
		
		attr_reader :id
		attr_reader :name
		attr_reader :type
		attr_reader :level
		attr_reader :effect_id
		attr_reader :upgrade_gold
		attr_reader :upgrade_require_level
		attr_reader :monster_max_hp
		attr_reader :monster_attack
		attr_reader :monster_movespeed
		attr_reader :attacker_movespeed_down
		attr_reader :icon
		
		def initialize
			@id = 0
			@name = ""
			@type = 0
			@level = 0
			@effect_id = 0
			@upgrade_gold = 0
			@upgrade_require_level = 0
			@monster_max_hp = 0
			@monster_attack = 0
			@monster_movespeed = 0.0
			@attacker_movespeed_down = 0.0
			@icon = ""
		end
		
		
		def write_to_stream(os)
			
			os.write_int32(@id)
			os.write_string(@name)
			os.write_string(@icon)
			os.write_int16(@type)
			os.write_int16(@level)
			os.write_int32(@upgrade_gold)
			os.write_int32(@monster_max_hp)
			os.write_int32(@monster_attack)
			os.write_float(@monster_movespeed.to_f)
			os.write_float(@attacker_movespeed_down.to_f)
			
			write_next_level_to_stream(os)
		end
		
		def write_next_level_to_stream(os)
			
			# 找到下一级属性
			next_level_tech = HomeTechnology.find_tech(@type, :by_type=>true, :by_level=>@level + 1)
			
			if(next_level_tech)
				os.write_byte(1)

				os.write_int32(next_level_tech.monster_max_hp)
				os.write_int32(next_level_tech.monster_attack)
				os.write_float(next_level_tech.monster_movespeed.to_f)
				os.write_float(next_level_tech.attacker_movespeed_down.to_f)
				
			else
				os.write_byte(0)
			end
			
		end
		
		class << self
			
			@@home_tech = {}
			def reload_templete()
				
				@@home_tech = {}
				CSV.load_csv("res/tmp_home_tech.csv") do |hash|
					tech = new()
					tech.init_from_hash(hash);
					@@home_tech[tech.id] = tech
				end
			end
			
			def techs_templete
				return @@home_tech;
			end
			
			#==========================================================================================
			# => 通过key找到科技
			#==========================================================================================
			def find_tech(key, oper={ :by_id=>true, :by_type=>false, :by_level=>nil, :by_mutil=>false })
			
				@techs ||= []
			
				ret = nil
				for k, t in @@home_tech
					cond = true
				
					if(oper[:by_id])
						cond &= t.id == key
					end
					if(oper[:by_type])
						cond &= (t.type == key)
					end
					if(oper[:by_level] != nil)
						cond &= (t.level == oper[:by_level])
					end
				
					if(cond)
					
						if( oper[:by_mutil] )
							ret ||= []
							ret << t
						else
							return t		
						end
					
					end
				
				end
			
				return ret
			end
		
			
		end
		
	end

end