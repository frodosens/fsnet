
require 'game/modules/systems/game_system.rb'

class AIOSystem < GameSystem


	attr_reader :game_players
	def initialize
		@game_players = {}
	end

	def join( channel )
		@game_players[channel.owner.game_player.pid] = channel
	end

	def leave( channel )
		@game_players.delete(channel.owner.game_player.pid)
	end

	def get_all
		@game_players.values
	end

	def update(dt)
		super
	end

	def shutdown
		super

	end


end