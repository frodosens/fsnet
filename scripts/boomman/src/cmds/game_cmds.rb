
require 'pack/pack.rb'
require 'cmds/boomman_pack_type.rb'
require 'cmds/cmd_property_update.rb'



class AgentNode
	
	attr_accessor :pid
	
	def player
		if(@pid == nil)
			return nil
		end
		return $game_players.find_player_by_pid(@pid);
	end

	#==========================================================================================
	# => 提示
	#==========================================================================================
	def tips(msg, serial=0)
		GameCMDS::send_tips(self, msg, serial)
	end

	#==========================================================================================
	# => 消息窗口
	#==========================================================================================
	def msgbox(msg, serial=0)
		GameCMDS::send_tips(self, msg, serial, :type => CMDTips::TIPS_TYPE_MSGBOX)
	end
	
end


module GameCMDS
	
	
	SUCCESS = 1	# 成功
	FAIL    = 0	# 失败
	
	def self.send_tips(sender, msg, serial, oper={ :type => CMDTips::TIPS_TYPE_TOP, :code=>0 })
		
		oper[:serial] = serial;
		sender.send_pack( CMDTips.create(msg, oper) )
		
	end
	
	def cmd_response_msg(request_pack, node, code, msg)
		
		os.write_byte(code)
		os.write_string(msg)
		pack = Pack.create( request_pack.serial, PACK_TYPE_MESSAGE, os );
		node.send_pack(pack);
		
	end
	
	# => 协议号	:PACK_TYPE_REGIST_SESSION
	# => 注册一个session
	def cmd_regiest_session(sender, pack)
		
		os = FSOutputStream.new();
		
		now = Time.now
		session_id = sender.id 
		time       = now.to_i
		
		os.write_int32( session_id );
		os.write_int64( time );
		
		pack = Pack.create( pack.serial,  PACK_TYPE_REGIST_SESSION, os);
		
		sender.send_pack(pack);
		
	end
	
	# => 协议号	:PACK_TYPE_VERSION
  # => 获取版本
	def cmd_version(sender, pack)

		max_version  = pack.input.read_byte();		# 
		mind_version = pack.input.read_byte();
		min_version  = pack.input.read_byte();
		
		os = FSOutputStream.new()				# 
		
		os.write_byte(max_version);
		os.write_byte(mind_version);
		os.write_byte(min_version);
		
	  response = Pack.create( pack.serial, PACK_TYPE_VERSION, os );
		
	  sender.send_pack(response);
		
	end
	

	# => 协议号	:PACK_TYPE_AUTHORIZE
	# => 获取登陆授权ID
	def cmd_authorize(sender, pack)
		
		user_name = pack.input.read_string();
		user_pwd = pack.input.read_string();
		
		user = $game_users.find_user_by_account( user_name, user_pwd );
		os = FSOutputStream.new()				# 
		
		if(user != nil)
			user.cache();
			os.write_byte(SUCCESS);
			
		
			key = User.generate_cache_key( user_name, user_pwd )
			authorize_code = key
		
			
			os.write_string(authorize_code);
		else
			os.write_byte(FAIL);
		end
		

	  response = Pack.create( pack.serial, PACK_TYPE_AUTHORIZE, os );
	  sender.send_pack(response);
		
		
	end
  
	# => 协议号
	# => 注册
	def cmd_regist( sender, pack )
		
		
		user_name = pack.input.read_string();
		user_pwd = pack.input.read_string();
		user_uuid = pack.input.read_string();
		user = $game_users.regist_user( user_name, user_pwd, user_uuid );
		
		os = FSOutputStream.new()				# 
		
		if(user != nil)
			os.write_byte(SUCCESS);
			os.write_string(user_name);
			os.write_string(user_pwd);
		else
			os.write_byte(FAIL);
		end
	  response = Pack.create( pack.serial, PACK_TYPE_REGIST, os );
	  sender.send_pack(response);
		
	end
	
	# => 协议号	:PACK_TYPE_LOGIN
	# => 登陆
	def cmd_login(sender, pack)
		
		authorize = pack.input.read_string();
		
		player = nil;
		user = $game_users.find_user_by_authorize(authorize);
		if(user != nil)
			player = $game_players.login(user.pid);
		end
		os = FSOutputStream.new()				
		
		if(user != nil && player != nil)
			os.write_byte(SUCCESS)
			player.write_to_stream(os);
		else
			os.write_byte(FAIL)
		end
		
		sender.pid = player.pid;
		player.client = sender;
		
		
	  response = Pack.create( pack.serial, PACK_TYPE_LOGIN, os );
	  sender.send_pack(response);
		
	end
    
	# => 协议号	:PACK_TYPE_INIT_PLAYER
	# => 初始化人物
	def cmd_init_player(sender, pack)
		
		player = sender.player
		
		# 0-man 1 woman
		sex = pack.input.read_byte();
		# name
		name = pack.input.read_string();
		
		name = name.force_encoding("UTF-8")
		
		os = FSOutputStream.new()
		if(sex != 0 and sex != 1)
			os.write_byte(FAIL);
		else
			if(player.sex == -1)
				os.write_byte(SUCCESS);
				player.init(sex, name);
				player.write_to_stream(os);
				player.cache();
			else
				os.write_byte(FAIL);
			end			
		end	
		
		
		response = Pack.create( 0, PACK_TYPE_INIT_PLAYER, os );
	  sender.send_pack(response);
			
		
		response = Pack.create( pack.serial, PACK_TYPE_INIT_PLAYER, os );
	  sender.send_pack(response);
			
	end
	
    
	# => 协议号	:PACK_TYPE_CHAT_MSG
	# => 聊天
	def cmd_chat_msg(sender, pack)
		
		type = pack.input.read_byte();
		msg  = pack.input.read_string();
		
		# 委托到协议内
		CMDChatMsg.execute(sender, self, type, msg)
		
	end
    
	# => 协议号	:PACK_TYPE_MAIL
	# => 邮件
	def cmd_mail(sender, pack)
		
		type = pack.input.read_byte()
		serial = pack.input.read_uint32();
		
		case type
		when CMDMail::CMD_MAIL_RECV_DELETE			# 删除邮件
			CMDMail.execute_del(pack.serial, sender, self, serial);
		when CMDMail::CMD_MAIL_READ						# 设置已读
			CMDMail.execute_read(pack.serial, sender, self, serial);
		end
	end
	
	
	# => 协议号	:PACK_TYPE_ENTER_MAP
	# => 进入PVE地图
	def cmd_enter_map(sender, pack)
		
		map_id = pack.input.read_int16()
		hero_serial = pack.input.read_uint32()
		CMDEnterMap.execute(pack.serial, sender, self, map_id, hero_serial)
		
	end
	
	
	# => 协议号	:PACK_TYPE_EXECUTE_SQL
	# => 执行SQL	
	def cmd_execute_sql(sender, pack)
		
		sql_count = pack.input.read_int16
		sqls = []
		for i in 0...sql_count
			sqls << pack.input.read_string()
		end
		
		CMDDBExecute.execute_sqls(sqls)
		
	end
	
	# => 协议号	:PACK_TYPE_HOME
	# => 家园系统
	def cmd_home(sender, pack)
		if($game_homes != nil)
			$game_homes.cmd_home(sender, pack)
		end
	end

	
	# => 协议号	:PACK_TYPE_GEMS
	# => 宝石系统
	def cmd_gems(sender, pack)
		if($game_gems != nil)
			$game_gems.cmd_gems(sender, pack)
		end
	end
	
	# => 协议号	:PACK_TYPE_BATTLE_RESULT
	# => 验证PVE战斗结果
	def cmd_battle_result(sender, pack)
		
		battle_level     = pack.input.read_int16()
		battle_time      = pack.input.read_float()
		battle_ext_hp    = pack.input.read_int32()
		battle_log_count = pack.input.read_int32()

		
		logs = []
		for i in 0...battle_log_count
			logs << PVELog.create_from_is(pack.input)
		end
		
		CMDBattleResult.execute(pack.serial, sender, self, battle_level, battle_time, battle_ext_hp, logs)
		
	end
	
	# => 协议号	:PACK_TYPE_SYCN_FILE
	# => 同步文件
	def cmd_sycn_file(sender, pack)
		
		file_name = pack.input.read_string()
		file_md5	 = pack.input.read_string()
		CMDSycnFile.execute(sender, file_name, file_md5, pack.serial)
		
	end
	
	
	
end