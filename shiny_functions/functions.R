process_text_to_audio_and_save <- function(input_text,output_file){
  
  base_url <- "http://localhost:59125/process"
  
  query_params <- list(
    INPUT_TYPE ="TEXT",
    OUTPUT_TYPE="AUDIO",
    INPUT_TEXT = input_text,
    effect_Whisper_selected = "off",
    effect_Whisper_parameters = "amount%3A100.0%3B",
    effect_Whisper_default="Default",
    effect_Whisper_help="Help",
    AUDIO_OUT="WAVE_FILE",
    VOICE_SELECTIONS="cmu-rms-hsmm%20en_US%20male%20hmm",
    LOCALE="en_US",
    VOICE="cmu-rms-hsmm",
    AUDIO="WAVE_FILE"
  )
  
  response = GET(url = base_url, query = query_params)
  
  writeBin(content(response,"raw"),output_file)
}

play_macOS <- function(audio_file) {
  system(sprintf("open -a 'QuickTime Player' '%s'", audio_file))
}

apiFunction <- function(prompt,max_length){
  
  request_body <- toJSON(list(
    n=1,
    max_context_length=2048,
    max_length= max_length,
    rep_pen = 1.1,
    temperature = 1.5,
    top_p = 0.95,
    top_k = 40,
    top_a = 0 ,
    typical = 1,
    tfs = 1,
    rep_pen_range = 320 ,
    rep_pen_slope = 0.7,
    sampler_order = c(6,0, 1,3, 4, 2, 5),
    memory = "",
    mirostat = 2,
    mirostat_tau = 5,
    mirostat_eta = 0.25,
    min_p =0 ,
    genkey = "KCPP5602",
    prompt = prompt,
    quiet = TRUE,
    stop_sequence =c("You:","\\nYou ","\\Snake: ", "END", " END "),
    use_default_badwordsids = FALSE
  ), auto_unbox = TRUE, escape = TRUE)
  
  
  
  request <- POST(
    
    url = "http://localhost:5001/api/v1/generate",
    content_type = 'application/json',
    body = request_body
  )
  
  response <- content(request)$results[[1]]$text
  
  responseFormatted <- gsub('\n','',response)
  
  return(responseFormatted)
  
}


updateAndgetResponse <- function(userComment,max_length){
  
  conversation_history <<- c(conversation_history,paste0("\nYou: ", userComment, "\nSnake:"))
  
  apiResponse <- apiFunction(prompt = paste(conversation_history, collapse = ""), max_length = max_length)
  
  
  conversation_history <<- c(conversation_history, apiResponse)
  
  return(apiResponse)
  
}

generate_tts_url <- function(text, speaker_id, style_wav = "", language_id = ""){
  base_url <- "http://localhost:5002/api/tts?"
  text_param <- paste0("text=",URLencode(text))
  speaker_param <- paste0("speaker_id=",speaker_id)
  style_param <- ifelse(style_wav != "", paste0("style_wav=",style_wav),"")
  language_param <- ifelse(language_id != "", paste0("language_id=",language_id), "&") 
  
  
  url <- paste(base_url,text_param, speaker_param, style_param, language_param, sep= "&") 
  return(url)
  
  
}


talkWithAI <- function(currentMessage, maxLengthInput, voiceID){
  
  system("rm currentResponse.txt")
  pb <- txtProgressBar(min = 0, max = 100, style = 3)
  
  
  apiFunctionResponse <- updateAndgetResponse(userComment = currentMessage, max_length = maxLengthInput)
  setTxtProgressBar(pb, 50)
  
  
  #process_text_to_audio_and_save(input_text = apiFunctionResponse, output_file = './currentResponse.wav')
  #generate_tts_url()
  
  text <- apiFunctionResponse
  speaker_id <- voiceID
  tts_url <- generate_tts_url(text, speaker_id)
  
  response <- GET(tts_url)
  
  file_path <- "currentResponse.wav"
  
  download.file(url=tts_url, destfile = file_path, mode= "wb")
  
  setTxtProgressBar(pb, 100)
  close(pb)
  
  audio <- readWave("./currentResponse.wav")
  
  print(apiFunctionResponse)
  cat(apiFunctionResponse, file = "./currentResponse.txt", append = TRUE)
  
  #play(audio)
  play_macOS("currentResponse.wav")
  
  
}

chatWithAI <- function(currentMessage, maxLengthInput){
  
  system("rm currentResponse.txt")
  pb <- txtProgressBar(min = 0, max = 100, style = 3)
  
  
  apiFunctionResponse <- updateAndgetResponse(userComment = currentMessage, max_length = maxLengthInput)
  setTxtProgressBar(pb, 50)
  
  
  #process_text_to_audio_and_save(input_text = apiFunctionResponse, output_file = './currentResponse.wav')
  #generate_tts_url()
  
  # text <- apiFunctionResponse
  # speaker_id <- "p229"
  # tts_url <- generate_tts_url(text, speaker_id)
  # 
  # response <- GET(tts_url)
  # 
  # file_path <- "currentResponse.wav"
  # 
  # download.file(url=tts_url, destfile = file_path, mode= "wb")
  # 
  # setTxtProgressBar(pb, 100)
  # close(pb)
  # 
  # audio <- readWave("./currentResponse.wav")
  
  print(apiFunctionResponse)
  cat(apiFunctionResponse, file = "./currentResponse.txt", append = TRUE)
  # 
  # #play(audio)
  # play_macOS("currentResponse.wav")
  
  
}


greeting <- function(init_prompt,userFirstMessage,maxLengthInput){
  system("rm greeting.txt")
  
  apiFirstOutput <- updateAndgetResponse(userComment = paste0(init_prompt,'\nYou: ',userFirstMessage,'\nSnake: '), 
                                         max_length = maxLengthInput)
  print(apiFirstOutput)
  #process_text_to_audio_and_save(input_text=apiFirstOutput,output_file = './greeting.wav')
  
  text <- apiFirstOutput
  speaker_id <- "p301"
  tts_url <- generate_tts_url(text, speaker_id)
  
  response <- GET(tts_url)
  
  file_path <- "greeting.wav"
  
  download.file(url=tts_url, destfile = file_path, mode= "wb")
  
  
  audio <- readWave("./greeting.wav")
  
  cat(apiFirstOutput, file = "./greeting.txt", append = TRUE)
  
  #play(audio)
  play_macOS("greeting.wav")
}


render_full_convo <- function(){
  
  process_text_to_audio_and_save(input_text = str_c(conversation_history,collapse=''), output_file = './fullConvo.wav')
  
  
  audio <- readWave("./fullConvo.wav")
  
  print(conversation_history)
  
  play_macOS("fullConvo.wav")
  
}



greetingVoice <- function() {
  system("sox ./greetingR.wav -r 16000 ./greetingR_16k.wav")
  system("sox greetingR_16k.wav -b 16 greetingR_16bit.wav")
  system("sox greetingR_16bit.wav -b 16 -c 1 greetingR_16bit_p.wav")
  
  
  greetingToSend <- system('curl localhost:8080/inference -H "Content-Type: multipart/form-data" -F file=@"./greetingR_16bit_p.wav" -F response_format="json" | jq',
                           intern=T)[2]
  
  cleaned_string <- gsub('.*"(.*?)"\\s*$', '\\1', greetingToSend)
  
  cleaned_string <- gsub("\\\\n", "", cleaned_string)
  
  system("rm greetingR.wav")
  system("rm greetingR_16k.wav")
  system("rm greetingR_16bit.wav")
  system("rm greetingR_16bit_p.wav")
  
  
  
  return(cleaned_string)
}

process_recorded_message <- function(desiredLength, voiceID) {
  system("sox ./test.wav -r 16000 ./test_16k.wav")
  system("sox test_16k.wav -b 16 test_16bit.wav")
  system("sox test_16bit.wav -b 16 -c 1 test_16bit_p.wav")
  
  currentMessageToSend <- system('curl localhost:8080/inference -H "Content-Type: multipart/form-data" -F file=@"./test_16bit_p.wav" -F response_format="json" | jq',
                                 intern=T)[2]
  
  cleaned_string <- gsub('.*"(.*?)"\\s*$', '\\1', currentMessageToSend)
  
  cleaned_string <- gsub("\\\\n", "", cleaned_string)
  
  system("rm test.wav")
  system("rm test_16k.wav")
  system("rm test_16bit.wav")
  system("rm test_16bit_p.wav")
  
  talkWithAI(currentMessage = cleaned_string, maxLengthInput= desiredLength, 
             voiceID = voiceID)
}

talkConvo <- function(inputText, speakerInput,filePathName){
  
  text <- inputText  
  speaker_id <- speakerInput
  tts_url <- generate_tts_url(text, speaker_id)
  response <- GET(tts_url)
  #file_path <- paste0("./combineWav/testConvo",i,".wav")
  file_path <- filePathName
  download.file(url=tts_url, destfile = file_path, mode= "wb")
  audio <- readWave(file_path)
  play_macOS(file_path)
}

renderFullConvoSpeech <- function(){
  
  #
  
  
  for(i in 1:length(conversation_history)){
    conversation_history[i]
    textFormatted <- gsub("You:","",conversation_history[i])  
    textFormatted <- gsub("\n","",textFormatted)  
    textFormatted <- gsub("Snake:","",textFormatted)  
    
    file_path <- paste0("./combineWav/testConvo",i,".wav")
    
    print(textFormatted)
    
    if(i%%2 == 0 ){
      
      talkConvo(as.character(textFormatted), "p307",filePathName =  file_path)
    } else {
      
      talkConvo(textFormatted, "p253",filePathName =  file_path)
    }
  }
  

}