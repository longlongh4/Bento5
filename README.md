# Bento5

Bento5 is a hackathon project based on Bento4 to generate better packaging results for HLS.

We used to use Bento4 to generate HLS v3 streams. But sometimes we get playback issues like video skips and repeated scenes.

After doing some research, I think most of these are caused by bad segment alignment.

With Bento4, we can merge different bitrates of MP4 streams into one HLS stream, so the player could switch bitrates during playback. Ideally, all segments should be aligned. But sometimes H264 encoder will insert keyframes at different positions according to scene-detect result for different resolutions. Bento4 will generate an HLS stream for each mp4 file independently, so it is hard for it to make sure all segments are aligned.


![](./imgs/segment_alignment.png)

Smart players can sync to the correct PTS before playback, some players will not, so video jumps.

mp42hls in Bento4 only accepts one MP4 file as the input, it is impossible to align for different bitrates. So I created a new tool named mov2hls to replace that.