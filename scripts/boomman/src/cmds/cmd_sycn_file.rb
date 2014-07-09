
require 'cmds/boomman_pack_type.rb'

class GameManagerResoucre
	
	alias :sycn_file_clear_cache :clear_cache
	def clear_cache()
		CMDSycnFile.clear_cache
		sycn_file_clear_cache()
	end
	
end

class CMDSycnFile < Pack
	
	@@cache_file = {}
	@@cache_md5  = {}
	
	class << self
		
		SYCN_FILE_SYCNED	  = 0		# 文件已同步
		SYCN_FILE_NO_EXISTS = 1		# 文件不存在
		SYCN_FILE_DOWNLOAD  = 2		# 开始下载
		
		def clear_cache()
			@@cache_file = {}
			@@cache_md5 = {}
		end
		
		def execute(sender, file_name, file_md5, require_serial)
			
			os = FSOutputStream.new
			
			path = "res/#{file_name}"
			
			if(FileTest.exists?(path))
				
				# 先測試從cache中讀取
				data = @@cache_file[file_name]
				md5  = @@cache_md5[file_name]
				if(data.nil?)
					data = File.read(path)
				  md5 = MD5Util.md5(data)
					@@cache_file[file_name] = data
					@@cache_md5[file_name] = md5
				end
				
				
				# 如果MD5相同
				if(file_md5 == md5)
					os.write_byte(SYCN_FILE_SYCNED)
				else
					os.write_byte(SYCN_FILE_DOWNLOAD)
					os.write_string(file_name)
					os.write_string(data)
				end
			else
				# 文件不存在
				os.write_byte(SYCN_FILE_NO_EXISTS)
			end
			
			pack = Pack.create( require_serial, PACK_TYPE_SYCN_FILE, os )
			sender.send_pack(pack)
		
		end
		
	end
	
end
