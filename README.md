To run the composite backend of
- Kobold CPP (NeuralBeagle 7B) on port 5001
- Coqui TTS on port 5002
- WhisperCPP on port 8080

Run 
```
chmod 555 run_entire_build.sh
./run_entire_build.sh
```

To stop
```
chmod 555 prune_entire_build.sh
./prune_entire_build.sh
```

The user-facing application right now is a POC, just a simple Rshiny app that interfaces between the backends. It is built for MacOS right now as it considers the inbuilt 'rec' command to record audio input.

A simple port can be modified for Windows using a software like ffmpeg. Still tbd for linux audio device recording.

All APIs run independently of the Rshiny app, which is NOT packaged with the docker compose build. Simply install R and the dependencies listed in /rshiny_deps Dockerfile to set up the environment for the front end. This is more a philosophical interlay of technologies than a true working POC

Initial Greeting speech input
![image](https://github.com/snakewizardd/speechToSpeechLLM/assets/83378208/e3656ea0-bd3c-4690-ae72-b5d0f60bd0d1)

Follow up Message speech input
![image](https://github.com/snakewizardd/speechToSpeechLLM/assets/83378208/7192a650-6b2f-457f-b39b-1edc613f8e4f)
