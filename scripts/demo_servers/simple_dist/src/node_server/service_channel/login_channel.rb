
class LoginChannel < ChannelBase

	def get_player_from_account(user_name, user_pwd)

	end

	def login(user_name, user_pwd)

		login_ret(true)

	end

	define_rpc(:init)
	define_rpc(:login_ret)

end