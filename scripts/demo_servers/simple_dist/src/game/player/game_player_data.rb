

module Player
	class GamePlayerData

		@@_user_data = {}
		class << self

			alias :_attr_name_attr_reader :attr_reader

			alias :_attr_name_attr_accessor :attr_accessor
			def attr_reader(*attr_names)
				for attr_name in attr_names
					@@_user_data[self] ||= []
					@@_user_data[self] << attr_name
				end
				_attr_name_attr_reader(*attr_names)
			end

			def attr_accessor(*attr_names)
				for attr_name in attr_names
					@@_user_data[self] ||= []
					@@_user_data[self] << attr_name
				end
				_attr_name_attr_accessor(*attr_names)
			end

		end


		def to_hash

			ret = Hash.new

			if !@@_user_data.include? self.class
				@@_user_data[self.class] = []
			end
			for attr_name in @@_user_data[self.class]

				ret[attr_name] = self.instance_variable_get("@#{attr_name}")

			end

			ret

		end

	end
end
