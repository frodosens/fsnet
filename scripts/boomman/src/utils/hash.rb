

class HashCode
	
	class << self
		def hash(string)
			return OpenSSL::HMAC.hexdigest("SHA1", "boomman", string).to_i(16) % 0xffffffff
		end
	end
	
end


require 'digest/md5'


class MD5Util
	
	class << self
			
		def md5(str)
			return Digest::MD5.hexdigest(str)
		end
		
	end
	
end