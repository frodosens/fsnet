require 'pathname'
require 'iconv'

class String
   def to_gb
     Iconv.conv("gb2312//IGNORE","UTF-8//IGNORE",self)
   end
   def utf8_to_gb
     Iconv.conv("gb2312//IGNORE","UTF-8//IGNORE",self)
   end
   def gb_to_utf8
     Iconv.conv("UTF-8//IGNORE","GB18030//IGNORE",self)
   end
   def to_utf8
     Iconv.conv("UTF-8//IGNORE","GB18030//IGNORE",self)
   end
	 def utf8?    
	     unpack('U*') rescue return false    
	     true	    
	 end  
end

def conver(ext, extname)
	file_count = 0;
	prog = 0;
	for path in Dir[ext]
		unless ( FileTest.directory?(path) )
			if(File.extname(path) == extname)
				file_count += 1
			end
		end
	end
	for path in Dir[ext]
		unless ( FileTest.directory?(path) )
			if(File.extname(path) == extname)
				file = File.open(path, "rb");
				data = file.read
				file.close
				data.gsub!("\r\n", "\n");
				file = File.open(path, "wb")
				file.write(data.utf8? ? data : data.gb_to_utf8)
				file.close
				prog += 1
				print "(#{prog}/#{file_count}) #{path} is done \n";
			end
		end
	end
	
end



conver("*", ".csv");
