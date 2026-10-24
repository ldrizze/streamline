# M0: monitor capture -> AV1 (AMF, 8 Mbps CBR) -> RTP pay/depay -> D3D11 decode -> window.
# Prints rendered/dropped/fps. ~5 s at 60 fps.
param([int]$Frames = 300, [int]$BitrateKbps = 8000)
$b = 'C:\msys64\ucrt64\bin'; $env:PATH = "$b;$env:PATH"
& "$b\gst-launch-1.0.exe" -e d3d11screencapturesrc num-buffers=$Frames show-cursor=true '!' `
  'video/x-raw(memory:D3D11Memory),framerate=60/1' '!' d3d11convert '!' `
  'video/x-raw(memory:D3D11Memory),format=NV12,width=1920,height=1080' '!' `
  amfav1enc usage=low-latency rate-control=cbr bitrate=$BitrateKbps gop-size=120 '!' av1parse '!' `
  rtpav1pay '!' rtpav1depay '!' av1parse '!' d3d11av1dec '!' `
  fpsdisplaysink video-sink=d3d11videosink text-overlay=false sync=false -v 2>&1 |
  Select-String 'ERROR|rendered' | Select-Object -Last 3
