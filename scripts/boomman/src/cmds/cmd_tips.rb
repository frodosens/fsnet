require 'cmds/boomman_pack_type.rb'
class CMDTips < Pack
	
	TIPS_TYPE_TOP		 = 0	# 头顶消息
	TIPS_TYPE_MSGBOX = 1	# 消息窗口
	
	def version
		return 0;
	end
	
	class << self
		
		def create(msg, oper={ :type => TIPS_TYPE_TOP, :serial=>0, :code=>0 })
			
			
			os = FSOutputStream.new();
			os.write_byte(oper[:type])
			os.write_byte(oper[:code])
			os.write_string(msg)
			
			return Pack.create( oper[:serial], PACK_TYPE_TOP_TIPS, os );
		
		end
		
	end
	
	
end