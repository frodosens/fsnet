$fix_hash = {};

$fix_hash["D1001"] = "D001"




def fix_qutily(filename)
  if(FileTest.directory?(filename))
    for subFile in Dir[filename + "/*"]
		if(subFile == "." or subFile == "..")
			next
		end	
		
	p subFile
      fix_qutily(subFile)
    end
    return;
  end
  
  
  if(filename.split(".")[1] != 'tmx')
    return;
  end
  
  file = File.open(filename, "rb");
  
  data = file.read();
  
  for key in $fix_hash.keys
    data.gsub!(key, $fix_hash[key]);
  end
  
  file.close();
  
  
  
  _filename = filename.split(".")[0];
  _file_ext = filename.split(".")[1];
  file = File.open(filename, "wb");
  
  file.write(data);
  
  file.close();
end

for file in Dir["*"]
  fix_qutily(file);
end