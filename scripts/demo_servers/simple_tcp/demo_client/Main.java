package com.sens;

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

	/**
	 * @param args
	 * @throws IOException 
	 * @throws UnknownHostException 
	 */
	public static void main(String[] args) throws UnknownHostException, IOException {
		
		Socket socket = new Socket("127.0.0.1", 50560);
		OutputStream os = socket.getOutputStream();
		DataOutputStream dos = new DataOutputStream(os);
		
		byte byte_order = 0;
		int full_len = 0;
		int serial = 0;
		short package_type = 1;// PACK_TYPE_HELLO
		byte version = 0;
		short make_sum = 0;
		byte[] data = { 1, 1, 0 };
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
		
		
		dos.write(buff.array());
		dos.flush();
		
		InputStream is = socket.getInputStream();
		DataInputStream dis = new DataInputStream(is);
		
		
		byte_order = dis.readByte();
		full_len = dis.readInt();
		serial = dis.readInt();
		package_type = dis.readShort();
		version = dis.readByte();
		make_sum = dis.readShort();
		data_len = htol(dis.readInt());
		dis.read(data, 0, data_len);
		System.out.println(String.format("%d,%d,%d", data[0], data[1], data[2]));
		
	}
	
	public static int htol(int v){
		return (v >> 24 & 0xff) | (v >> 16 & 0xff) << 8 | (v >> 8 & 0xff) << 16 | (v & 0xff) << 24;
	}

}
