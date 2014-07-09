
class CSV
	
	class << self
	
		def load_csv(csv_name, oper={ :first_type=>false }, &proc)
			row_index = 0;
			reader = CSV.open(csv_name) do |csv|
				if(oper[:first_type])
					csv.shift;
				end
				titles = []
				csv.shift.each do |title|
					titles << title
				end
				row_index += 1
				csv.each do |cols|
					row_hash = {}
					title_index = 0
					cols.each do |col|
						row_hash[ titles[title_index] ] = col
						title_index += 1;
					end
					yield row_hash, row_index;
				end
			end
		end
	end
	
end