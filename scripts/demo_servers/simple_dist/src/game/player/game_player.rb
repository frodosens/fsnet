
require 'game/player/player_data/equips.rb'
require 'game/player/player_data/items.rb'
require 'game/player/player_data/aio.rb'


module Player


	class GamePlayer

		attr_reader :pid
		attr_reader :name
		attr_reader :player_data

		def initialize(id)
			@pid = id
			@name = ""
			@player_data = {}

			require_data(:equip, Equips)
			require_data(:items, Items)
			require_data(:aio, AIO)

		end

		def require_data(data_name, data)
			@player_data[data_name] = data.new

			script = "  def #{data_name} ; return @player_data[:#{data_name.to_sym}]; end  "
			instance_eval(script)

		end

		def save

		end

		def cache

		end

		def to_hash

			ret = {
					:pid => @pid,
					:name => @name
			}

			for name, data in @player_data

				ret[name] = data.to_hash

			end

			ret

		end


	end

end
