# require 'uuidtools'
class UUID

	class << self

		def generate
			return rand(0xffffffff).to_s
			# return UUIDTools::UUID.random_create.to_s
		end


	end

end