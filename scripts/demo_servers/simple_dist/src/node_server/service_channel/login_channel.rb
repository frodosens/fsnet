
require 'game/player/game_player.rb'

class LoginChannel < ChannelBase

	def get_player_from_account(user_name, user_pwd)

	end

	def login(user_name, user_pwd, table)

		game_player = Player::GamePlayer.new(owner.id)

		self.owner.init_player(game_player)

		login_ret( true, self.owner.game_player.to_hash )


		# send aio channel
		aio_channel = owner.create_channel( AIOChannel, "AIOChannel", self.local_owner )
		owner.send_channel(aio_channel)
		aio_channel._init

	end

	define_rpc(:init)
	define_rpc(:login_ret)

end