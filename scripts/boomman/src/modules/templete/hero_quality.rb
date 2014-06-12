
class Hero
	
	# 英雄品质属性模板
	class HeroQualityLevel
		attr_reader :quality
		attr_reader :value
	
		def initialize()
			@quality = 0
			@value = 0
		end
	
	
		def init_from_hash(hash)
	
			@quality = hash["qualitylevel_level"].to_i
			@value = hash["qualitylevel_value"].to_i
	
		end
	
		def self.create_from_hash(hash)
			model = HeroQualityLevel.new()
			model.init_from_hash(hash);
			return model
		end
	
	end
	
	
end