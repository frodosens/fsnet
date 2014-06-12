
class Hero
	

	# 英雄等级属性模板
	class HeroLevel
	
		attr_reader :level
		attr_reader :max_hp
		attr_reader :attack
		attr_reader :defence
		attr_reader :move_speed
		attr_reader :bomb_num
		attr_reader :bomb_range
		attr_reader :bomb_reload
	
		def initialize()
			@level = 0
			@max_hp = 0
			@attack = 0
			@defence = 0
			@move_speed = 0.0
			@bomb_num = 0
			@bomb_range = 0;
		end
	
		def init_from_hash(hash)
			@level = hash["level"].to_i;
			@max_hp = hash["level_hp_limit_add"].to_i
			@attack = hash["level_attack_add"].to_i
			@defence = hash["level_defence_add"].to_i
			@move_speed = hash["level_movespeed_mut"].to_i
			@bomb_num = hash["level_maxbombnum_mut"].to_i
			@bomb_range = hash["level_maxbombrange_mut"].to_i
			@bomb_reload = hash["level_bombreload_mut"].to_i
		end
	
		def self.create_from_hash(hash)
		
			tmp = HeroLevel.new()
			tmp.init_from_hash(hash);
			return tmp;
			
		end
	
	end

end
