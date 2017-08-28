-- the output directory may be "hardcoded" this simple way,
-- but if you use command line (tshark) and thus you can set
-- environment variables, use
-- local outputdir = os.getenv("my_output_path")
-- as a way to fetch the path from an environment
-- variable "my_output_path" instead

--          
driver = require "luasql.postgres"
env = assert (driver.postgres())
con = assert (env:connect("voiplog","dbworker","vFcnbh_+"))
-- 
local outputdir = "/data/pcaps"

-- declare the Lua table for file handles
local files = {}

-- declare the Lua table of frames containing SDPs
local sdp_frames = {}

-- declare the Lua table of rtp data files
local rtp_files = {}

-- prepare the field extractors for the individual protocol types which we are tapping
local frame_number_f = Field.new("frame.number")

local rtp_setup_frame_f = Field.new("rtp.setup-frame")

local t38_setup_frame_f = Field.new("t38.setup-frame")

local rtcp_setup_frame_f = Field.new("rtcp.setup-frame")


local sip_callid_f = Field.new("sip.Call-ID")
local sip_method_f = Field.new("sip.Method")
local sip_to_tag_f = Field.new("sip.to.tag")
local sip_status_code_f = Field.new("sip.Status-Code")
local sip_request_line_f = Field.new("sip.Request-Line")
local sip_from_addr_f = Field.new("sip.from.addr")
local sip_to_addr_f = Field.new("sip.to.addr")
local sip_cseq_method_f = Field.new("sip.CSeq.method")
local sdp_connection_info_address_f = Field.new("sdp.connection_info.address")
local sdp_media_f = Field.new("sdp.media")
local rtp_p_type_f = Field.new("rtp.p_type")
local rtp_ssrc_f = Field.new("rtp.ssrc")
local rtcp_ssrc_identifier_f = Field.new("rtcp.ssrc.identifier")
local udp_src_f = Field.new("udp.srcport")
local udp_dst_f = Field.new("udp.dstport")
local ip_src_f = Field.new("ip.src")
local ip_dst_f = Field.new("ip.dst")

local sdp_version_f = Field.new("sdp.version")

local rtp_payload_f = Field.new("rtp.payload")
local rtp_seq_f = Field.new("rtp.seq")

-- create and register the listener
local tap = Listener.new("ip", "rtp or rtcp or t38 or (sip and !(sip.CSeq.method == REGISTER) and !(sip.CSeq.method == OPTIONS))")

-- declare the executive body of the tap
function tap.packet(pinfo,tvb,ip)

-- declare a common function handling all media-like packets
  function handle_media(setup_frame)
    -- if a setup frame for this media stream has actually been encountered, save the packet
    if sdp_frames[setup_frame] then
      files[sdp_frames[setup_frame]]:dump_current()
    end
  end

-- attempt to extract all signature values
  local frame_number = frame_number_f().value -- I can do it this because frame.number always exists
  local sip_callid = sip_callid_f()
  local sip_method = sip_method_f()
  local sip_to_tag = sip_to_tag_f()
--
  local sip_status_code = sip_status_code_f()
  local sip_request_line = sip_request_line_f()
  local sip_from_addr = sip_from_addr_f()
  local sip_to_addr = sip_to_addr_f() 
  local sip_cseq_method = sip_cseq_method_f() 
--
  local udp_src = udp_src_f()
  local udp_dst = udp_dst_f()
  local ip_src = ip_src_f()
  local ip_dst = ip_dst_f()
--
  local sdp_version = sdp_version_f()
  local sdp_connection_info_address = sdp_connection_info_address_f()
  local sdp_media = sdp_media_f()
-- 
  local rtp_setup_frame = rtp_setup_frame_f()
  local rtp_p_type = rtp_p_type_f()
  local rtp_ssrc = rtp_ssrc_f()
  local rtp_payload = rtp_payload_f()
  local rtp_seq = rtp_seq_f() 
--
  local rtcp_setup_frame = rtcp_setup_frame_f()
  local rtcp_ssrc_identifier = rtcp_ssrc_identifier_f()
  local t38_setup_frame = t38_setup_frame_f()

-- handle SIP packets
  if sip_callid then
    sip_callid_v = sip_callid.value

-- check whether the PDU is an initial INVITE, and create a call if it is and if that call doesn't exist yet
-- because there was an unauthorized initial INVITE before
    sip_method = sip_method_f()
    if sip_method then
      if (sip_method.value == "INVITE" and not(sip_to_tag_f()) and not(files[sip_callid_v])) then
        local f_handle = Dumper.new_for_current( outputdir .. "/" .. tostring(sip_callid) ..".pcap" )
        files[sip_callid_v] = f_handle
	res = assert (con:execute(string.format("INSERT INTO cdr (clid, src, dst, userfield)  VALUES ('%s', '%s', '%s', '%s');", 
	              tostring(sip_callid), tostring(sip_from_addr), tostring(sip_to_addr), tostring(sdp_connection_info_address).." ; "..tostring(sdp_media) ) ))
      end
    end

-- check whether the PDU contains an SDP and if so, add the frame to the list
-- of those responsible for media stream establishment
    if files[sip_callid_v] then
      if sdp_version then
        sdp_frames[frame_number] = sip_callid_v
      end
    end

-- finally, if the frame belongs to an existing call, copy it to the output file
    local f_handle = files[sip_callid_v]
    if f_handle then
      f_handle:dump_current()
      if ((tostring(sip_cseq_method) == "BYE" and tostring(sip_status_code) == "200") or 
          (tostring(sip_cseq_method) == "INVITE" and tostring(sip_status_code) == "487") ) then
	 f_handle:flush()
         f_handle:close()
	 f_handle = nil
	 files[sip_callid_v] = nil
         if(rtp_files[sip_callid_v]) then
	   for k,rtp_f in pairs(rtp_files[sip_callid_v]) do
	      rtp_f[1]:flush()
	      rtp_f[1]:close()
	      os.execute("./G711orG729-2wav ".. outputdir .. "/" .. tostring(sip_callid_v) .."_"..tostring(k).."."..tostring(rtp_f[2]).." "..tostring(rtp_f[2]))
              --os.execute("rm ".. outputdir .. "/" .. tostring(k) .."_"..tostring(l).."."..tostring(rtp_f[2]).." "..tostring(rtp_f[2]))
	      rtp_f=nil
	   end
	 end
         rtp_files[sip_callid_v]=nil
	 for k,v in pairs(sdp_frames) do
	   if v==sip_callid_v then
	     sdp_frames[k]=nil
	   end
	 end
      end
    end
  end

-- handle "media" packets
  if rtp_setup_frame then
    handle_media(rtp_setup_frame.value)
    if sdp_frames[rtp_setup_frame.value] then
      if rtp_ssrc and rtp_p_type then
        local first_key = sdp_frames[rtp_setup_frame.value]
	local second_key = rtp_ssrc.value
	if not(rtp_files[first_key]) then
	   rtp_files[first_key]={}
	end
	local rtp_file_handle
	if not(rtp_files[first_key][second_key]) then
           local my_rtp_name = tostring(first_key).."_"..tostring(second_key).."."..tostring(rtp_p_type.value)
	   rtp_file_handle = assert(io.open(outputdir .. "/" ..my_rtp_name, "wb"))
	   rtp_files[first_key][second_key]={rtp_file_handle,rtp_p_type.value}
	   print( my_rtp_name )
	else
	   rtp_file_handle = rtp_files[first_key][second_key][1]
	end
        if (rtp_file_handle and rtp_payload) then
	  rtp_file_handle:write(rtp_payload.value:raw())
	end
      end
    end
  end

  if rtcp_setup_frame then
    handle_media(rtcp_setup_frame.value)
  end

  if t38_setup_frame then
    handle_media(t38_setup_frame.value)
  end

end

-- declare the function to print the progress, not actually necessary
function tap.draw()
  for call_id,f_handle in pairs(files) do
    f_handle:flush()
    f_handle:close()
    f_handle=nil
    files[call_id]=nil
  end
  for k,v in pairs(rtp_files) do
     for l,rtp_f in pairs(v) do
        rtp_f[1]:flush()
        rtp_f[1]:close()
	os.execute("./G711orG729-2wav ".. outputdir .. "/" .. tostring(k) .."_"..tostring(l).."."..tostring(rtp_f[2]).." "..tostring(rtp_f[2]))
        --os.execute("rm ".. outputdir .. "/" .. tostring(k) .."_"..tostring(l).."."..tostring(rtp_f[2]).." "..tostring(rtp_f[2]))
        rtp_f=nil
     end
     rtp_files[k]=nil
  end
  for k,v in pairs(sdp_frames) do
     if v==sip_callid_v then
       sdp_frames[k]=nil
     end
  end

  -- Close the database connection
  con:close()
  env:close()
end

-- declare what to do after the last packet has been processed
function tap.reset()
  -- close all files at once here, which may be way too late if there are hundreds of calls
  -- and so you may run out of your file handle quota
  for call_id,f_handle in pairs(files) do
    f_handle:flush()
    f_handle:close()
    f_handle=nil
    files[call_id]=nil
  end
  for k,v in pairs(rtp_files) do
     for l,rtp_f in pairs(v) do
        rtp_f[1]:flush()
        rtp_f[1]:close()
	os.execute("G711orG729-2wav ".. outputdir .. "/" .. tostring(k) .."_"..tostring(l).."."..tostring(rtp_f[2]).." "..tostring(rtp_f[2]))
        rtp_f=nil
     end
     rtp_files[k]=nil
  end

  --for call_id,ty in pairs(my_rtp_types) do
  --     os.execute("mv ".. outputdir .. "/" .. tostring(call_id) ..".pcap "..outputdir .. "/" .. tostring(call_id).."_"..tostring(ty)..".pcap ")
  --     my_rtp_types[call_id]=nil
  --end
  -- Close the database connection
  con:close()
  env:close()

end