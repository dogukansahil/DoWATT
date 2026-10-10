// Copyright (c) 2026 Dogukan Sahil
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission: see NVML-EXCEPTION.txt.
// Build-time Windows resource encoder. No generated helper is shipped in the EXE.
using System;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Collections.Generic;
public static class DoWATTResources {
    static void Align(BinaryWriter w) { while ((w.BaseStream.Position & 3) != 0) w.Write((byte)0); }
    static void Key(BinaryWriter w, string text) { w.Write(Encoding.Unicode.GetBytes(text + "\0")); }
    static byte[] Block(string key, ushort type, byte[] value, ushort valueLength, params byte[][] children) {
        using (var stream = new MemoryStream()) using (var w = new BinaryWriter(stream)) {
            w.Write((ushort)0); w.Write(valueLength); w.Write(type); Key(w, key); Align(w);
            w.Write(value); foreach (var child in children) { Align(w); w.Write(child); }
            byte[] result = stream.ToArray(); if (result.Length > ushort.MaxValue) throw new InvalidDataException("Version block too large");
            result[0] = (byte)result.Length; result[1] = (byte)(result.Length >> 8); return result;
        }
    }
    static byte[] Version(string rc, string version) {
        var parts = version.Split('.'); uint major = uint.Parse(parts[0]), minor = uint.Parse(parts[1]), patch = uint.Parse(parts[2]);
        byte[] fixedInfo; using (var stream = new MemoryStream()) using (var w = new BinaryWriter(stream)) {
            foreach (uint n in new uint[] {0xFEEF04BD,0x10000,(major<<16)|minor,patch<<16,(major<<16)|minor,patch<<16,63,0,0x40004,1,0,0,0}) w.Write(n);
            fixedInfo = stream.ToArray();
        }
        var fields = new List<byte[]>();
        foreach (Match m in Regex.Matches(File.ReadAllText(rc), @"VALUE\s+""([^""]+)"",\s*""([^""]*)""")) {
            string value = m.Groups[2].Value; fields.Add(Block(m.Groups[1].Value,1,Encoding.Unicode.GetBytes(value+"\0"),(ushort)(value.Length+1)));
        }
        foreach (string key in new [] {"FileVersion","ProductVersion"}) fields.Add(Block(key,1,Encoding.Unicode.GetBytes(version+"\0"),(ushort)(version.Length+1)));
        byte[] strings = Block("StringFileInfo",1,new byte[0],0,Block("040904b0",1,new byte[0],0,fields.ToArray()));
        byte[] translation = {9,4,176,4};
        byte[] vars = Block("VarFileInfo",1,new byte[0],0,Block("Translation",0,translation,4));
        return Block("VS_VERSION_INFO",0,fixedInfo,52,strings,vars);
    }
    static void Resource(BinaryWriter w, ushort type, ushort id, ushort language, byte[] bytes, ushort flags) {
        Align(w); w.Write((uint)bytes.Length); w.Write((uint)32);
        w.Write((ushort)65535); w.Write(type); w.Write((ushort)65535); w.Write(id);
        w.Write((uint)0); w.Write(flags); w.Write(language); w.Write((uint)0); w.Write((uint)0); w.Write(bytes); Align(w);
    }
    public static void Write(string destination, string icoPath, string manifest, string rc, string version) {
        byte[] ico = File.ReadAllBytes(icoPath); if (ico.Length < 6 || BitConverter.ToUInt16(ico,2) != 1) throw new InvalidDataException("Invalid ICO");
        ushort count = BitConverter.ToUInt16(ico,4); if (count == 0 || ico.Length < 6+16*count) throw new InvalidDataException("Invalid ICO directory");
        using (var output = File.Create(destination)) using (var w = new BinaryWriter(output)) {
            Resource(w,0,0,0,new byte[0],0);
            byte[] group; using (var gs = new MemoryStream()) using (var g = new BinaryWriter(gs)) {
                g.Write((ushort)0); g.Write((ushort)1); g.Write(count);
                for (int i=0;i<count;i++) {
                    int at=6+i*16; uint length=BitConverter.ToUInt32(ico,at+8), start=BitConverter.ToUInt32(ico,at+12);
                    if (start > ico.Length || length > ico.Length-start) throw new InvalidDataException("ICO image outside file");
                    byte[] image=new byte[length]; Buffer.BlockCopy(ico,(int)start,image,0,(int)length);
                    Resource(w,3,(ushort)(i+1),1033,image,0x1030);
                    g.Write(ico,at,12); g.Write((ushort)(i+1));
                }
                group=gs.ToArray();
            }
            Resource(w,14,101,1033,group,0x1030);
            Resource(w,24,1,0,File.ReadAllBytes(manifest),0x1030);
            Resource(w,16,1,1033,Version(rc,version),0x1030);
        }
    }
}
