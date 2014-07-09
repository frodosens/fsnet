require 'modules/basemode.rb'
#==========================================================================================
#   邮件实例
# 	By Frodo	2014-06-11
#==========================================================================================
class Mail < BaseModule
	
	attr_reader :id							# 模板ID
	attr_reader :serial				 	# 邮件唯一序列号
	attr_reader :send_pid			 	# 发送者的ID	# 系统为0
	attr_accessor :send_name			# 发送者的名字
	attr_reader :send_date			# 发送的日期
	attr_accessor :recv_pid			 	# 接受者的ID
	attr_accessor :unread				 	# 是否未读
	attr_reader :title				 	# 邮件标题
	attr_reader :content				# 邮件内容
	attr_reader :attachment1_id	# 附件道具1ID
	attr_reader :attachment1_count	# 附件道具1数量
	attr_reader :attachment2_id	# 附件道具2ID
	attr_reader :attachment2_count	# 附件道具2数量
	attr_reader :attachment3_id	# 附件道具3ID
	attr_reader :attachment3_count	# 附件道具3数量
	attr_reader :attachment4_id	# 附件道具4ID
	attr_reader :attachment4_count	#附件道具4数量
	attr_reader :deleted				# 是否已删除		

	def initialize
		super()
		@id = 0
		@serial = 0
		@send_pid = 0
		@recv_pid = 0
		@unread = 1
		@title = ""
		@content = ""
		@attachment1_id = 0
		@attachment1_count = 0
		@attachment2_id = 0
		@attachment2_count = 0
		@attachment3_id = 0
		@attachment3_count = 0
		@attachment4_id = 0
		@attachment4_count = 0
		@deleted = 0
		@send_date = Time.now.to_i
		@send_name = ""
		
	end
	
	
	#==========================================================================================
	# => 是否未读
	#==========================================================================================
	def unread?
		return @unread == 1
	end
	
	#==========================================================================================
	# => 设置是否未读
	#==========================================================================================
	def unread=(v)
		@unread = v ? 1 : 0
	end
	
	
	#==========================================================================================
	# => 生成入库的sql
	#==========================================================================================
	def generate_save_sql()
		cols = "serial, send_pid, send_name, send_date, recv_pid, unread, title, content, attachment1_id, attachment1_count, attachment2_id, attachment2_count, attachment3_id, attachment3_count, attachment4_id, attachment4_count, deleted"
		vls  = "#{@serial}, #{@send_pid}, '#{@send_name}', #{@send_date}, #{@recv_pid}, #{@unread}, '#{@title}', '#{@content}', #{@attachment1_id}, #{@attachment1_count}, #{@attachment2_id}, #{@attachment2_count}, #{@attachment3_id}, #{@attachment3_count}, #{@attachment4_id}, #{@attachment4_count}, #{@deleted} "
		sql = "replace into tb_mail(#{cols}) values( #{vls} )" 
		return sql
	end
	
	#==========================================================================================
	# => 写入输出流
	#==========================================================================================
	def write_to_stream(os)
		
		
	end
	
	class << self
		
		@@mail_templetes = {}
		def reload_templete
			@@mail_templetes = {}
			CSV.load_csv("res/tmp_mail.csv") do |hash|
				mail = Mail.new()
				@@mail_templetes[hash["id"].to_i] = mail.init_from_hash(hash);
			end
		end
		
		def create_from_templete(tmp_id)
			mail = @@mail_templetes[tmp_id]
			if(mail == nil)
				return nil;
			end
			mail = mail.deep_clone
			mail.instance_eval {
				@serial = $game_database.incr("incr_mail_serial")
				@send_date = Time.now.to_i
				@send_name = "System"
			}
			
			return mail;		
		end
		
	end
	
end