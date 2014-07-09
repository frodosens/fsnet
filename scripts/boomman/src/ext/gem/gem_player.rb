
class GameGemSystem < GameSystemBase

	class GemDataResource
	
		attr_reader :gems_holes	
	
		def initialzie
			@gems_holes = []
		end
	
		def method_missing(method_name, *arg, &block)
			value = 0
			
			for id in gems_holes
				if(id != 0)
					item = Item.find_templete(id)
					gem_effect = GameGemSystem::GemEffect.find_effect( item.tmp_gems_effect )
					if(gem_effect)
						method = gem_effect.method(method_name)
						value += method.call()
					end
				end
			end
			
			return value	
		end
	
	
		# 寶石組
		def gems_holes
			if(@gems_holes.nil?)
				@gems_holes = Array.new(gems_holes_beopened_count, 0)
			end
			while(@gems_holes.size > gems_holes_beopened_count)
				@gems_holes.pop
			end
			while(@gems_holes.size < gems_holes_beopened_count)
				@gems_holes << 0
			end
			return @gems_holes
		end
	
		# 已開啟控數量
		def gems_holes_beopened_count
			return $game_gems.configure["gem_hole_count"]
		end
	
		# 是否已鑲嵌這種類型的寶石
		def exist_gem_by_type(type)
			for id in @gems_holes
				if(id > 0)
					if(Item.find_templete(id).subtype == type)
						return true
					end
				end
			end
			return false
		end
	
		# 裝備寶石
		def set_gem(index, id, player)
		
			return false if(index >= @gems_holes.size || index < 0)
		
			item = find_item(id);
			return false if(item.nil? and id != 0)
			# 检查同类型宝石
			return false if(exist_gem_by_type(item.subtype))
		
			if(@gems_holes[index] != 0)
				player.gain_item(@gems_holes[index], 1)
			end
			player.lose_item(id)
			@gems_holes[index] = id
		
		
			return true
		end
	
	end

end


class Player
	
	attr_reader :gems

	alias :old_gem_initialze :initialize
	
	def initialize()
		old_gem_initialze();
		@gems = GameGemSystem::GemDataResource.new
	end
	
	def gems
		if(@gems.nil?)
			@gems = GameGemSystem::GemDataResource.new
		end
		return @gems
	end
	

	def set_gem(index, id)
		return gems.set_gem(index, id, self);
	end
		
		
end