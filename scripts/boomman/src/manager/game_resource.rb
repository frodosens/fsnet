
class GameManagerResoucre
	
	attr_reader :items;
	
	def initialize()
		reload();
	end
	
	def clear_res()
		
	end
	
	def reload_items()
		
		@items.clear();
		
		CSV.foreach("items.csv") do |row|
			
				
			
		end
		
	end
	
	
	
	def reload()
		reload_items();
		
	end
	
	
	
end

GR = GameManagerResoucre