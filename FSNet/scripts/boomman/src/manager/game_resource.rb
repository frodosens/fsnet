
class CSV
	
	class << self
	
		def load_csv(csv_name, &proc)
			reader = CSV.open(csv_name) do |csv|
				titles = []
				csv.shift.each do |title|
					titles << title
				end

				csv.each do |cols|
					row_hash = {}
					title_index = 0
					cols.each do |col|
						row_hash[ titles[title_index] ] = col
						title_index += 1;
					end
					yield row_hash;
				end
			end
		end
	end
	
end

class GameManagerResoucre
	
	attr_reader :items_templete;	# 道具模板
	attr_reader :heros_templete;	# 英雄模板
	
	alias :items :items_templete
	alias :heros :heros_templete
	
	def initialize()
		@items_templete = {};
		@heros_templete = {};
		
		reload();
	end
	
	
	def reload_items()
		
		@items_templete.clear();
		
		Item.reload_templete();
	
	end
	
	def reload_heros()
		@heros_templete.clear();
		
		Hero.reload_templete();
		
	end
	
	
	def reload()
		reload_items();
		reload_heros();
		
	end
	
	
	
end
