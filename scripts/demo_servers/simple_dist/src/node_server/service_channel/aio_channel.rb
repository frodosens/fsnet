
require 'game/player/game_player.rb'
require 'game/modules/game_module.rb'

class AIOChannel < ChannelBase

	def on_create(data)
		@aio_system = local_owner.game_module.get_system(AIOSystem)

	end

	def join(x, y)

		aio = owner.game_player.aio
		aio.x = x
		aio.y = y

		@aio_system.join(self)

		for other_channel in @aio_system.get_all
			other_channel.other_join(owner.game_player.to_hash)
		end


	end

	def move(x, y)
		aio = owner.game_player.aio
		aio.x = x
		aio.y = y

		for other_channel in @aio_system.get_all
			other_channel.other_move(owner.game_player.pid, x, y)
		end

	end

	def leave
		@aio_system.leave(self.owner)

	end

	def _init

		dates = []

		for other_channel in @aio_system.get_all
			dates << other_channel.owner.game_player.to_hash
		end

		init(dates)

	end

	define_rpc(:init)
	define_rpc(:other_join)
	define_rpc(:other_move)
	define_rpc(:other_leave)

end