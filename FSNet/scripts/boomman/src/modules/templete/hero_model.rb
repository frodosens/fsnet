
class Hero
	

	# 英雄模型模板
	class HeroModel
		attr_reader :id
		attr_reader :name
		attr_reader :arm_id_bone
		attr_reader :arm_id_dress
		attr_reader :arm_id_bomb
		attr_reader :arm_png_bomb
		attr_reader :arm_effect_bomb
		attr_reader :arm_effect_burn
		attr_reader :arm_bomb_scale
	
		def initialize
			@id = 0
			@name = ""
			@arm_id_bone = 0
			@arm_id_dress = 0
			@arm_id_bomb = 0
			@arm_id_bomb = ""
			@arm_png_bomb = ""
			@arm_effect_bomb = ""
			@arm_effect_burn = ""
			@arm_bomb_scale = 1.0
		end
	
		def init_from_hash(hash)
	
			@id = hash["model_id"].to_i
			@name = hash["model_name"]
			@arm_id_bone = hash["model_armaturename_id_bone"].to_i;
			@arm_id_dress = hash["model_armaturename_id_dress"].to_i;
			@arm_id_bomb = hash["model_armaturename_id_bomb"].to_i;
			@arm_png_bomb = hash["model_armaturename_png_bomb"];
			@arm_effect_bomb = hash["model_armaturename_effect_bomb"];
			@arm_effect_burn = hash["model_armaturename_effect_burn"];
			@arm_bomb_scale = hash["model_armaturename_bomb_scale"].to_f;
	
		end
	
		def self.create_from_hash(hash)
			model = HeroModel.new()
			model.init_from_hash(hash);
			return model
		end
	
	
	end

	
	
end