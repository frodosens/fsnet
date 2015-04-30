
class LoginChannel < ChannelBase

	def get_player_from_account(user_name, user_pwd)

	end

	def login(user_name, user_pwd)
		self.local.creaet_channel( nil, nil , self.local_server )
	end

	define_rpc(:init)

end