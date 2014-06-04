
require 'pack/pack.rb'
require 'cmds/boomman_pack_type.rb'
require 'cmds/cmd_property_update.rb'

module GameCMDS
	
	
	# => 协议号	:PACK_TYPE_REGIST_SESSION
	# => 注册一个session
	def regiest_session(sender, pack)
		
		os = FSOutputStream.new();
		
		now = Time.now
		session_id = sender.id 
		time       = (now.tv_sec * 1000 * 1000) + now.tv_usec

		os.write_int32( session_id );
		os.write_int64( time );
		
		pack = Pack.create( pack.serial,  PACK_TYPE_REGIST_SESSION, os);
		
		sender.send_pack(pack);
		
	end
	
	# => 协议号	:PACK_TYPE_VERSION
  # => 获取版本
	def version(sender, pack)

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
	def authorize(sender, pack)
		
		
	end
  
	
	# => 协议号	:PACK_TYPE_LOGIN
	# => 登陆
	def login(sender, pack)
		
		
		
	end
    
	
	
end