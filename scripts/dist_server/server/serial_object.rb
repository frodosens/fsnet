
require 'yaml'

module SerialObject

	def _serial
		return Marshal.dump(self)
	end

	class << self

		def _from_serial(str)
			Marshal.load(str)
		end

	end

end