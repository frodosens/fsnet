
class SafeSql
	
	
	class << self
		
		
		def conver(val)
			
			val = val.to_s().gsub("'") do |g|
				g = "\\'"
			end
			
			return val;
			
		end
		
		
	end
	
	
end