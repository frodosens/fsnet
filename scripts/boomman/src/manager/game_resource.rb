

class GameManagerResoucre
	
	def initialize()
		
	end
	
	def reload_items()
		return [Item, Hero, Player, Mail, Map, Monster, ModelTemplete]
	end
	
	def clear_cache
	end
	
	def reload()
		
		clear_cache()
		
		classes = reload_items
		
		cur_progr = 0
		
		for c in classes
			cur_progr += 1;
			c.reload_templete() do |c2|
				yield c.to_s, cur_progr, classes.length, c2
			end
			if(block_given?)
				yield c.to_s, cur_progr, classes.length
			end
		end
		
	end
	
	
	
end
