library(shiny)
library(shinybusy)

#Libraries
library(httr)
library(jsonlite)
library(dplyr)
library(readr)
library(tuneR)
library(stringr)

conversation_history <- c()


source('./shiny_functions/functions.R')



ui <- fluidPage(
  titlePanel("Voice Recording App"),
  selectInput("recordType", "Select Record Type", choices = c("greeting", "continuedMessage")),
  actionButton("startRecording", "Start Recording"),
  actionButton("stopRecording", "Stop Recording"),
  numericInput("responseLength", "Desired Response Length", value = 128, min = 1, max = 512, step = 1),
  actionButton("sendToAPI", "Send to API"),
  textOutput("recordingStatus"),
  textOutput("apiResponse"),
  add_busy_spinner()
)

server <- function(input, output, session) {
  recording <- FALSE
  currentRecording <- NULL
  
  observeEvent(input$startRecording, {
    if (!recording) {
      recording <<- TRUE
      if (input$recordType == "greeting") {
        currentRecording <<- "greetingR.wav"
      } else if (input$recordType == "continuedMessage") {
        currentRecording <<- "test.wav"
      }
      # Start recording
      system(paste0("rec -r 16000 ", currentRecording, " &"))
      output$recordingStatus <- renderText("Recording started")
    }
  })
  
  observeEvent(input$stopRecording, {
    if (recording) {
      recording <<- FALSE
      # Stop recording
      system(paste0("pkill -f 'rec -r 16000 ", currentRecording, "'"))
      output$recordingStatus <- renderText("Recording stopped")
    }
  })
  
  observeEvent(input$sendToAPI, {
    if (!is.null(currentRecording)) {
      # Call the appropriate function based on recording type
      if (currentRecording == "greetingR.wav") {
        api_response <- greeting(init_prompt = read_file('./prompts/init_alt.txt'), 
                                 userFirstMessage= greetingVoice(), maxLengthInput = input$responseLength)
        
        renderResponse <- read_file('./greeting.txt')
        renderResponse <- as.character(renderResponse)
        
      } else if (currentRecording == "test.wav") {
        api_response <- process_recorded_message(desiredLength = input$responseLength,
                                                 voiceID = "p301")
        
        renderResponse <- read_file('./currentResponse.txt')
        renderResponse <- as.character(renderResponse)
        
      }
      output$apiResponse <- renderText(renderResponse)
    }
  })
}

shinyApp(ui = ui, server = server)
