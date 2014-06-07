class GameUserManager
	
	def initialize()
	end
	
	
	def find_user_by_authorize( authorize )
		
		user = User.create_from_redis_key( authorize );
		return user;
		
	end
	
	def find_user_by_account( user_name, user_pwd )
		
		user = User.create_from_redis(user_name, user_pwd);
		
		# 从 database 生成
		if(user == nil)
			user = User.create_from_database( user_name, user_pwd );
		end
		
		return user;
		
	end
	
	def regist_user( user_name, user_pwd, user_uuid )
		
		
		begin
			uid = $game_database.execute("insert into tb_user( user_name, user_pwd, uuid ) values( '#{user_name}', '#{user_pwd}', '#{user_uuid}' ) ");
			pid = uid + 10000;
			$game_database.execute("insert into tb_player( pid, level, sex, name, morale, ap, exp, gold, diamonds, prestige, guild_id ) 
																		values( #{pid}, 0, -1, '', 0, 0, 0, 0, 0, 0, 0 ) ");
		rescue Mysql2::Error => err
			uid = -1
			print err;
		end
		if(uid > 0)
			
			user = User.new( uid, pid, user_name, user_pwd, user_uuid ); 
			user.cache();
			
			return user;
			
		end
		
		
		return nil;
		
	end
	
	
end