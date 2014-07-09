def createHD(file_name)
    
    file = File.open(file_name,'r');
    
    fileHD = open(file_name.split(".")[0] + "-hd.tmx", "w")
    
    while(!file.eof?)
        
        data = file.readline()
        
        if(data[/<map/] != nil or data[/<tileset/] != nil)
            
            data.gsub!("tilewidth=\"32\"", "tilewidth=\"64\"")
            data.gsub!("tileheight=\"32\"", "tileheight=\"64\"")
            
        end
        
        if(data[/<image/] != nil)
            data.gsub!(/source=".+?"/) { |e| e = e.split(".")[0] + "-hd." + e.split(".")[1] }
            data.gsub!(/width=".+?"/) { |e| "width=\""+(e.split("=")[1].gsub("\"", "").to_i * 2).to_s + "\"" }
            data.gsub!(/height=".+?"/) { |e| "height=\""+(e.split("=")[1].gsub("\"", "").to_i * 2).to_s + "\"" }
        end
        
        if(data[/<object/] != nil)
            data.gsub!(/x=".+?"/) { |e| "x=\""+(e.split("=")[1].gsub("\"", "").to_i * 2).to_s + "\"" }
            data.gsub!(/y=".+?"/) { |e| "y=\""+(e.split("=")[1].gsub("\"", "").to_i * 2).to_s + "\"" }
            data.gsub!(/width=".+?"/) { |e| "width=\""+(e.split("=")[1].gsub("\"", "").to_i * 2).to_s + "\"" }
            data.gsub!(/height=".+?"/) { |e| "height=\""+(e.split("=")[1].gsub("\"", "").to_i * 2).to_s + "\"" }
        end
        
        fileHD.write(data)
        
    end
    
    
    fileHD.close();
    file.close();
    
    print "#{file_name} is Done \n"
end


files =  Dir["*"]

for f in files
    if(f["-hd"] == nil && f.split(".")[-1] == "tmx")
        createHD(f)
    end
end

p 'All Done'
