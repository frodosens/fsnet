package com.sens;

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class Main {


	static final short PACKAGE_VERSION = 1;
	static final short PACKAGE_TIME = 2;
	/**
	 * @param args
	 * @throws IOException 
	 * @throws UnknownHostException 
	 */
	public static void main(String[] args) throws UnknownHostException, IOException {
		
		Socket socket = new Socket("127.0.0.1", 40000);
		OutputStream os = socket.getOutputStream();
		DataOutputStream dos = new DataOutputStream(os);
		
		InputStream is = socket.getInputStream();
		DataInputStream dis = new DataInputStream(is);
		
		
		
		byte[] data = { 1, 1, 0 };
		ByteBuffer buff = create_package(data, PACKAGE_VERSION);
		dos.write(buff.array(), 0, buff.position());
		dos.flush();
		
		data = new byte[]{};
		buff = create_package(data, PACKAGE_TIME);
		dos.write(buff.array(), 0, buff.position());
		dos.flush();
		
		// read package_version
		data = read_pack(dis);
		System.out.println(String.format("version is %d,%d,%d", data[0], data[1], data[2]));
		
		// read package_time
		data = read_pack(dis);
		buff = ByteBuffer.wrap(data);
		System.out.println(String.format("server time is %d",  buff.getInt()));
		
		
		
	}
	
	public static byte[] read_pack(DataInputStream dis) throws IOException{
		byte byte_order = dis.readByte();
		int full_len = dis.readInt();
		int serial = dis.readInt();
		short package_type = dis.readShort();
		byte version = dis.readByte();
		short make_sum = dis.readShort();
		int data_len = htol(dis.readInt());
		byte[] data = new byte[data_len];
		dis.read(data, 0, data_len);
		return data;
	}
	
	public static ByteBuffer create_package(byte[] data, short pack_type){


		byte byte_order = 0;
		int full_len = 0;
		int serial = 0;
		short package_type = pack_type;// PACK_TYPE_VERSION
		byte version = 0;
		short make_sum = 0;
		int data_len = data.length;
		full_len = 1 + 4 + 4 + 2 + 1 + 2 + 4 + data_len;
		
		ByteBuffer buff = ByteBuffer.allocate(100);
		buff.order(ByteOrder.LITTLE_ENDIAN);
		buff.put(byte_order);
		buff.putInt(full_len);
		buff.putInt(serial);
		buff.putShort(package_type);
		buff.put(version);
		buff.putShort(make_sum);
		buff.putInt(data_len);
		buff.put(data);		
		
		return buff;
		
	}
	
	public static int htol(int v){
		return (v >> 24 & 0xff) | (v >> 16 & 0xff) << 8 | (v >> 8 & 0xff) << 16 | (v & 0xff) << 24;
	}

}
