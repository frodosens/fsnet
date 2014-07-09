class GameUserManager
	
	def initialize()
	end
	

	#==========================================================================================
	# => 通过验证码找到一个user
	#==========================================================================================
	def find_user_by_authorize( authorize )
		
		user = User.create_from_redis_key( authorize );
		return user;
		
	end

	#==========================================================================================
	# => 通过账号密码找到一个user
	# => 先在cache中找, 然后再到db中找
	#==========================================================================================
	def find_user_by_account( user_name, user_pwd )
		
		user = User.create_from_redis(user_name, user_pwd);
		
		# 从 database 生成
		if(user == nil)
			user = User.create_from_database( user_name, user_pwd );
		end
		
		return user;
		
	end
	

	#==========================================================================================
	# => 注册一个user
	# => 返回新建的user
	#==========================================================================================
	def regist_user( user_name, user_pwd, user_uuid )
		
		
		begin
			uid = $game_database.execute("insert into tb_user(pid, user_name, user_pwd, uuid ) values( 0, '#{user_name}', '#{user_pwd}', '#{user_uuid}' ) ");
			pid = uid + 10000
			$game_database.execute("insert into tb_player( pid, level, sex, name, morale, ap, exp, gold, diamonds, prestige, guild_id ) 
																		values( #{pid}, 0, -1, 'no name', 0, 0, 0, 0, 0, 0, 0 ) ");
			$game_database.execute("update tb_user set pid=#{pid} where uid=#{uid}");															
		rescue Mysql2::Error => err
			uid = -1
			$game.err(err);
		end
		if(uid > 0)
			
			user = User.new( uid, pid, user_name, user_pwd, user_uuid ); 
			user.cache();
			
			return user;
			
		end
		
		
		return nil;
		
	end
	
	
end