
class Hero

	# 英雄星级属性模板
	class HeroStarLevel
	
		attr_reader :level
		attr_reader :value
	
		def initialize()
			@level = 0
			@value = 0
		end
	
	
		def init_from_hash(hash)
	
			@level = hash["starlevel"].to_i
			@value = hash["starlevel_value"].to_i
	
		end
	
		def self.create_from_hash(hash)
			model = HeroStarLevel.new()
			model.init_from_hash(hash);
			return model
		end
	
	end

end