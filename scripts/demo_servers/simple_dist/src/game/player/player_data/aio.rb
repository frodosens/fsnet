require 'game/player/game_player_data.rb'


module Player

	class AIO < GamePlayerData

		attr_reader :map_id
		attr_accessor :x
		attr_accessor :y

		def initialize
			@map_id = 0
			@x = 0
			@y = 0
		end

	end

end
