
class Local
	
	class << self
		
		def str(msg)
			return msg
		end
		
	end
	
	
end

def L(s)
	return Local.str(s)
end