
class Hero

	# 英雄强化属性模板
	class HeroStreLevel
	
		attr_reader :level
		attr_reader :max_hp
		attr_reader :attack
		attr_reader :defence
		attr_reader :move_speed
		attr_reader :bomb_num
		attr_reader :bomb_range
		attr_reader :bomb_reload
		def initialzie
			@level = 0
			@max_hp = 0
			@attack = 0
			@defence = 0
			@move_speed = 0
			@bomb_num = 0
		end
	  
		def init_from_hash(hash)
			@level = hash["strelevel"].to_i;
			@max_hp = hash["strelevel_hpmilit_add"].to_i
			@attack = hash["strelevel_attack_add"].to_i
			@defence = hash["strelevel_defence_add"].to_i
			@move_speed = hash["strelevel_movespeed_mut"].to_f
			@bomb_num = hash["strelevel_maxbombnum_mut"].to_i
			@bomb_range = hash["strelevel_maxbombrange_mut"].to_i
			@bomb_reload = hash["strelevel_bombreload_mut"].to_i
		end
	
		def self.create_from_hash(hash)
			stre = HeroStreLevel.new
			stre.init_from_hash(hash)
			return stre;
		end
	
	end


end
