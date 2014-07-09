


require 'cmds/boomman_pack_type.rb'
class CMDMail < Pack
	
	
	CMD_MAIL_RECV            = 0	# 接受邮件
	CMD_MAIL_READ						 = 1	# 设置已读
	CMD_MAIL_RECV_DELETE     = 2  # 删除邮件
	

	
	class << self
		
		def execute_del(request_serial, sedner, server, serial)
			player = sender.player
			
			os = FSOutputStream.new();
			
			mail = player.find_mail_by_serial(serial)
			# 为空或者已读
			if(mail.nil?)
				os.write_byte(GameCMDS::FAIL)
			else
				os.write_byte(GameCMDS::SUCCESS)
				mail.deleted = true
			end
			
			pack = Pack.create( request_serial, PACK_TYPE_MAIL, os )
			sender.send_pack(pack);
		end
		
		def execute_read(request_serial, sender, server, serial)
			player = sender.player
			
			os = FSOutputStream.new();
			
			mail = player.find_mail_by_serial(serial)
			
			# 为空或者已读
			if(mail.nil? or (mail != nil and !mail.unread))
				os.write_byte(GameCMDS::FAIL)
			else
				
				mail.unread = false
				player.gain_item( mail.attachment1_id, mail.attachment1_count )
				player.gain_item( mail.attachment2_id, mail.attachment2_count )
				player.gain_item( mail.attachment3_id, mail.attachment3_count )
				player.gain_item( mail.attachment4_id, mail.attachment4_count )
				
				os.write_byte(GameCMDS::SUCCESS)
				os.write_byte(CMD_MAIL_READ)
				os.write_uint32(serial)
			end
			
			pack = Pack.create( request_serial, PACK_TYPE_MAIL, os )
			sender.send_pack(pack);
		end
		
		
		
		def create_recv_main(mail)
			
			os = FSOutputStream.new()
			os.write_byte(CMD_MAIL_RECV);
			mail.write_to_stream(os);
			
			Pack.create( 0, PACK_TYPE_MAIL, os )
	
		end
		
	end
	
end