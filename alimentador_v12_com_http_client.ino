

#include <Console.h> //imprime no console serial
#include <Bridge.h> //A comunicação entre o Arduino e Linux é feita através da biblioteca Bridge que facilita a comunicação entre os dois processadores através de uma ponte serial.
#include <Process.h>  //usado para consultar a dataHora do arduinoYun
#include <Ultrasonic.h> //usado para medir o nivel do dispenser
#include <EEPROM.h> //necessario para gravar os horarios na eeprom
#include <BridgeHttpClient.h> //faz as chamadas para a API
#include <TembooYunShield.h> //classe utilizada para enviar email
#include "TembooAccount.h" //contem as informações da conta temboo
#include <HttpClient.h> 
#include <HTTPClient.h>

#include <ArduinoJson.h> //usado para trabalharmos com json

// variavel que recebe o json deserealizado. Depois de deserealizado acessamos as keys do json atraves desta variavel para recuperar as informações
StaticJsonDocument<350> doc;

// utilizamos essas variaveis para fazer as chamadas GET/POST para a api
//BridgeHttpClient bridgeHttpClientGet;
//BridgeHttpClient bridgeHttpClientPost; --> passei para variavel local pois estava dandos problema apos o envio do json, ficava esperando uma resposta e demorava muito as vezes até perdia a resposta do servidor que no caso ele retorna {} quando é inserido um dado de sensor

//referente ao sensor de umidade e temperatura
#include <DHT.h>
#define DHTPIN 7
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);


//referente a balança
#include "HX711.h"
#define DT A1 //pino analogico A1
#define SCK A0 //pino analogico A0
HX711 balanca;

//referente ao motor e ponte-h
#define PWM_pin 5 //pino que envia o sinal PWM para a ponte-H L298N. 
#define IN3 9     //pino da  ponte-H L298N que define o sentido de rotação do motor
#define IN4 10    //pino da  ponte-H L298N que define o sentido de rotação do motor


//referente ao botao fisico para liberar racao
#define pinBotaoLiberaRacao 3    //pino conectado ao botao libera ração presente fisicamente no alimentador
bool statusBotaoLiberaRacao = false; //variavel que armazena o status botao fisico do alimentador se for true é porque foi pressionado

//pinos triger e echo do sensor ultrasonico
Ultrasonic ultrasonic(11, 12);


//referente ao processo que obtem data e hora do Linux do Arduino YUN
Process date;

//variavel que armazena quando foi a utlima realização das tarefas que devem ocorer de minuto em minuto, porem nesse caso checamos de 10 em 10 segundos se houve uma mudança de minuto
unsigned long dezEmDezSegundos = 0;
unsigned short ultimoMinuto = 0; 

//variavel que armazena quando foi a utlima realização das tarefas que devem ocorer de segundo em segundo
unsigned long segundoEmSegundo = 0;

//serial do alimentador, ele usa esse serial para buscar comandos para ele e tambem para enviar dados dos seus proprios sensores
const String serialAlimentador = "ABC123";

//variavel que armazena um JSON referentes aos dados da ultima liberação. Exemplo data que ocorreu, status: sucesso ou falha, quantidade requisitada e quantidade liberada
String ultimaLiberacao = "{}";

//variavel que armazena um JSON referentes ao dados de agendamento, nessa variavel está os horarios e a quantidade a ser liberada
String agendamento = "";

//referente ao teemboo para envio de emails
//nome de usuario do gmail
const String GMAIL_USER_NAME = "autopetfeeder.informer@gmail.com";
//senha do gmail
const String GMAIL_PASSWORD = "DC415@vm";
//destinatario
String TO_EMAIL_ADDRESS = "nulo";

void setup() {

  // Inicia a bridge responsavel pela comunicação via serial com o IDUINO
  pinMode(13, OUTPUT);
  digitalWrite(13, LOW);
  Bridge.begin(); //pode ficar bloqueado por até 2 segundos
  digitalWrite(13, HIGH);
  digitalWrite(13, LOW);

  //define como saida os pinos que acionam a ponte-H  L298N do motor
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  //define o pino 8 do pinBotaoLiberaRacao como entrada
  pinMode(pinBotaoLiberaRacao, INPUT);

  //adiciona uma interrupçao no pino do pinBotaoLiberaRacao, toda vez que ocorrer uma subida de nivel logico de 0v para 5v a função botaoPressionado será chamada
  attachInterrupt(digitalPinToInterrupt(pinBotaoLiberaRacao), botaoPressionado, RISING);

  //define o sentido de rotação do motor
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);

  //define o pino com o sensor infravermelho que monitora se o pote está cheio
  pinMode(2, INPUT);

  //referente a balança
  balanca.begin(DT, SCK);
  balanca.read();
  balanca.set_scale(487.91); //esse valor eu obtive fazendo a media de varias leituras com um peso conhecido 0.0020226
  balanca.tare();
  balanca.set_offset(balanca.read_average(50)); //usar teste_Balanca_4 para fazer a media do offset 183542

  //referente ao console (para poder ver os dados no monitor serial)
  Console.begin();

  //referente ao sensor de umidade e temperatura
  dht.begin();

  //recupera as informações da eeprom como os agendamento e os dados sobre a utlima liberação de ração para saber se ocorreu tudo bem
  recuperaEeprom();


}

void loop() {

  //ações que devem ocorrer de minuto em minuto
  //porem estou verificando de dez em dez segundos se o minuto mudou
  if (millis() - dezEmDezSegundos >= 10000) {
    dezEmDezSegundos = millis();
    date.begin("date");
    date.addParameter("+%M");
    date.run();
    String minuto;
    while (date.available() > 0) {
      minuto = date.readStringUntil('\n');
    }
    
    //verifica se mudou o minuto
    if(ultimoMinuto!=minuto.toInt()){
      ultimoMinuto=minuto.toInt();
      checaSeEhoraDeLiberarRacao();
      sendSensores();
    }
    
  }

  //ações que devem ocorrer de segundo em segundo
  if (millis() - segundoEmSegundo >= 1000) {
    segundoEmSegundo = millis();
    getComandos();
  }

  //checa se o botao físico liberaRacao foi pressionado, se TRUE então libere uma quantidade fixa de 50 gramas por pressionada
  if (statusBotaoLiberaRacao == true) {
    liberaRacao(50);
    statusBotaoLiberaRacao = false;
  }


  delay(100);
}


void sendSensores() {
  /* envia as informações dos sensores para a api no formato:
      {"nivel":"Vazio","umidade":"48.80","temperatura":"30.10","ultimaLiberacao":{"dataUltimaLiberacao":"22/10/2021 20:42:22","quantidadeSolicitada":"30","quantidadeLiberada":"-0.11","status":"erro"}}
   *  */

  //BridgeHttpClient bridgeHttpClientPost;
  HttpClient bridgeHttpClientPost;

  Console.println(F("inicio leitura de sensores"));
  Console.flush();
  //lembrando que a data estamos obtendo do linux que está rodando no Iduino atraves da classe Process
  date.begin("date");
  date.addParameter("+\"%m/%d/%Y %H:%M:%S\"");
  date.run();
  while (date.available() > 0) {
    //leia a data atual
    String data = date.readStringUntil('\n');
    //leitura do nivel do dispenser
    int nivelEmCentimetros = ultrasonic.read();
    String nivel = "";
    if (nivelEmCentimetros > 25) {
      nivel = "Vazio";
    } else {
      if (nivelEmCentimetros < 15) {
        nivel = "Cheio";
      } else {
        nivel = "Medio";
      }
    }
    //Leitura da umidade
    String umidade = (String)dht.readHumidity();
    //Leitura da temperatura (Celsius)
    String temperatura = (String)dht.readTemperature();


    //bridgeHttpClientPost.addHeader("Accept: application/json");
    //bridgeHttpClientPost.addHeader("Content-Type: application/json");
    bridgeHttpClientPost.setHeader("Accept: application/json");
    bridgeHttpClientPost.setHeader("Content-Type: application/json");

    //antes de criar o json com as informações dos sensores verificar a variavel ultimaLiberacao
    //se a ultima liberação estiver vazia, nunca houve uma liberação de ração entao formate a variavel ultimaLiberacao com o padrao de json vazio {}
    //if (ultimaLiberacao == "") {
    //  ultimaLiberacao = "{}";
    //}

    Console.println(F("metade da leitura de sensores"));
    Console.flush();

    //cria uma string formatada para json com os dados, pode se migrar para usar o StaticJsonDocument<350> doc;
    String jsondata = ("{\"serialalimentador\":\"" + serialAlimentador + "\",\"data\":" + data + ",\"sensores\":{\"nivel\":\"" + nivel + "\",\"umidade\":\"" + umidade + "\",\"temperatura\":\"" + temperatura + "\",\"ultimaLiberacao\":" + ultimaLiberacao + "}}");
    //descobre o tamanho da string json e adiciona +1
    int jsondata_len = jsondata.length() + 1;
    //cria uma array de char do tamanho da string jsondata+1
    char jsondataCharArray[jsondata_len];
    //converte a string jsondata para o array de char jsondataCharArray
    jsondata.toCharArray(jsondataCharArray, jsondata_len);
    //usa o metodo post para enviar os dados para a API
    Console.println(F("quase no fim da leitura de sensores"));
    Console.flush();
    bridgeHttpClientPost.post("http://177.44.248.45:3300/sensor", jsondataCharArray);
    //bridgeHttpClientPost.postAsync("http://177.44.248.45:3300/sensor", jsondataCharArray); //<- troquei para postAsync pois estava havendo um delay grande apos o bridgeHttpClientPost.post

    while (bridgeHttpClientPost.available() > 0) {
      char c = bridgeHttpClientPost.read();
      Console.print(c);
    }
    //Console.print(F("\nResponse Code: "));
    //Console.println(bridgeHttpClientPost.getResponseCode());
    Console.flush();

    bridgeHttpClientPost.flush();

    //Console.println("Informacoes lidas a serem enviadas: " + (String)jsondataCharArray);
    Console.println(F("informacoes de sensores enviadas"));
    Console.flush();

  }

}

void getComandos() {
  /*  Captura os comandos para o alimentador
       os comandos vem em formato de json presente na coluna "comando" da tabela Comando do banco
       os comandos podem ser:
       {"liberar":{"quantidade":"50"}} = libera uma quantidade especifica de 50 gramas
       {"dieta":[{"horario":"12:00","quantidadegramas":150},{"horario":"08:00","quantidadegramas":25},{"horario":"18:00","quantidadegramas":30}]} = define um novo agendamento de dieta para o alimentador
       {"limpar":{}} = limpa os agendamentos do alimentador
       {"envioDeEmail":{msilva10@universo.univates.br}} = envio de feedback por email para esse destinatario
   *   */

  //BridgeHttpClient bridgeHttpClientGet;
  HttpClient bridgeHttpClientGet;

  //cria uma string com a url de requisição de comando
  String stringRequisicao = ("http://177.44.248.45:3300/comando/" + serialAlimentador);
  //descobre o tamanho da string stringRequisicao e adiciona +1
  int stringRequisicao_len = stringRequisicao.length() + 1;
  //cria uma array de char do tamanho da string stringRequisicao_len
  char stringRequisicaoCharArray[stringRequisicao_len];
  //converte a string stringRequisicao para o array de char stringRequisicaoCharArray
  stringRequisicao.toCharArray(stringRequisicaoCharArray, stringRequisicao_len);
  //efetua a requisição da url criada com o serial deste alimentador
  bridgeHttpClientGet.get(stringRequisicaoCharArray);
  //Coletar o corpo da resposta e armazenar nesta string para conversão
  String response;
  while (bridgeHttpClientGet.available() > 0) {
    char c = bridgeHttpClientGet.read();
    response += c;
  }

  //descubra o tamanho da response e adiciona 1
  int response_len = response.length() + 1;
  //cria uma array de char do tamanho da string response+1
  char jsonCharArray[response_len];
  //converte a string response para o array de char jsonCharArray
  response.toCharArray(jsonCharArray, response_len);

  // desserializar JSON
  DeserializationError error = deserializeJson(doc, jsonCharArray);

  //verifica se conseguiu converter o json recebido
  if (error) {
    //se nao conseguiu imprime erro ao desserializar o json
    //se o json vier vazio, ou seja não há nenhum comando para o alimentador nesse momento ele tambem gera um erro informando que a resposta é Empty
    //Console.print(F("deserializeJson() failed: "));
    //Console.println(error.f_str());

  } else {
    //se conseguiu desserializar, obtenha o idComando e o proprio comando das keys "idcomando" e "comando"
    String idComando = doc["idcomando"];
    String comando   = doc["comando"];

    //Console.println("Comando recebido: " + comando);

    //respondendo a api que o comando foi recebido e ela pode marcar ele como executado
    //cria uma string apontando para a rota que seta o idComando recebido como um comando que foi executado
    String setaComoExecutado = "http://177.44.248.45:3300/comando/comandoexecutado/" + idComando;
    //descubra o tamanho da setaComoExecutado e adiciona 1
    int setaComoExecutado_len = setaComoExecutado.length() + 1;
    //cria uma array de char do tamanho da string setaComoExecutado+1
    char setaComoExecutadoCharArray[setaComoExecutado_len];
    //converte a string setaComoExecutado para o array de char setaComoExecutadoCharArray
    setaComoExecutado.toCharArray(setaComoExecutadoCharArray, setaComoExecutado_len);
    //faz um get na rota da API específica que marca o comando com o idComando recebido como comandoexecutado
    bridgeHttpClientGet.get(setaComoExecutadoCharArray);


    //Agora e hora de processar o que veio dentro do COMANDO, ou seja o que realmente o alimentador deve fazer. Primeiramente ele deve desserializar o Json que esta dentro da key comando
    //descubra o tamanho do json que veio como response armazenado na string comando e adiciona 1
    int response_len = comando.length() + 1;
    //cria uma array de char do tamanho da string response+1
    char jsonCharArray[response_len];
    //converte a string response para o array de char jsonCharArray
    comando.toCharArray(jsonCharArray, response_len);

    // desserializar JSON
    DeserializationError error = deserializeJson(doc, comando);

    //verifica se conseguiu converter o json que estava dentro de "comando"
    if (error) {
      //se nao conseguiu imprime erro ao desserializar o json
      Console.print(F("deserializeJson() failed of the comando: "));
      Console.println(error.f_str());
      Console.flush();

    } else {
      //se conseguiu desserializar, vamos ver se é um comando de despejar ração ou uma nova dieta ou limpar agendamentos
      if (doc.containsKey("liberar")) {
        int quantidade = doc["liberar"]["quantidade"];
        Console.println("Recebeu comando de LIBERAR a quantidade: " + (String)quantidade);
        Console.flush();
        liberaRacao(quantidade);
      }
      if (doc.containsKey("limpar")) {
        Console.println(F("Recebeu comando de LIMPAR agendamentos"));
        Console.flush();
        agendamento = "";
        //chama o metodo que vai gravar o agendamento vazio na eeprom
        gravaEeprom();
      }
      if (doc.containsKey("envioDeEmail")) {
        TO_EMAIL_ADDRESS = doc["envioDeEmail"]["email"].as<String>();
        Console.println("Recebeu comando de envioDeEmail, enviar para: " + TO_EMAIL_ADDRESS);
        Console.flush();
        //chama o metodo que vai gravar o novo email na eeprom
        gravaEeprom();
      }
      if (doc.containsKey("dieta")) {
        //Console.println("Recebeu comando de DIETA. A quantidade de refeições é: " + String(doc["dieta"].as<JsonArray>().size()));
        Console.println(F("Recebeu comando de DIETA"));
        Console.flush();

        //converte o json que esta dentro da chave "dieta" para a string agendamento
        //limpar a variavel agendamento antes de preenche-la com o novo agendamento
        agendamento = "";
        serializeJson(doc["dieta"], agendamento);

        //chama o metodo que vai gravar o novo agendamento na eeprom
        gravaEeprom();

      }


    }

  }
}


void liberaRacao(int gramas) {

  //armazena o status do resultado final da liberação OK ou ERRO
  String status = "";
  //zera a balança
  balanca.tare();

  //variavel booleana que controla se estourou o tempo para liberar ração, isso serve para o alimentador nao fica eternamente com o motor ligado esperando descer a ração se por exemplo a ração ficar trancada no dispenser. Por padrao o alimentador ira aguardar até 30s para que a quantidade de ração seja liberada
  bool erroTimeOut = false;
  //quandi inicia a liberação de ração.
  unsigned long momentoInicioLiberacao = millis();

  //capture a data e hora do momento em que essa liberação de ração começou
  date.begin("date");
  date.addParameter("+\"%d/%m/%Y %H:%M:%S\"");
  date.run();
  while (date.available() > 0) {

    //leia a data atual em que esta liberação de ração esta ocorrendo
    String data = date.readStringUntil('\n');
    //verifique o pino do sensor infravermelho, se for igual a 1 entao o pote esta vazio, caso contrario se for igual a 0 o sensor está acionado e o pote esta cheio
    if (digitalRead(2)) {
      //coloca a potencia maxima no pino pwm do motor
      analogWrite(PWM_pin, 255);
      //enquanto o que tem na balança for menor que a quantidade de gramas solicitada permaneça com o motor ligado
      while (balanca.get_units(2) < gramas) {

        if (millis() - momentoInicioLiberacao >= 30000) { //se estourar o timeout pare o loço pois ocorreu um problema, alimentador enguiçou
          erroTimeOut = true;
          break;
        }
        delay(1);
      }
      //desliga o motor
      analogWrite(PWM_pin, 0);

      if (erroTimeOut) {
        status = "erro - tempo excedido";
        sendMail("Houve um problema na liberacao de " + String(gramas) + "g de racao as " + data);
      } else {
        status = "OK";
        //se liberou com sucesso, faça uma leitura do nivel do dispenser, se estiver baixo dispare um email
        int nivelEmCentimetros = ultrasonic.read();
        if (nivelEmCentimetros > 25) {
          sendMail(F("O nivel do reservatorio está baixo"));
        }
      }
    } else {
      //o pino do sensor infravermelho esta com valor baixo (0v) o que significa que o pote está cheio de ração e uma nova liberação nao pode ocorrer
      status = "erro - pote cheio";
      sendMail("Houve um problema na liberacao de " + String(gramas) + "g de racao as " + data+", o pote estava cheio");
    }

    //preencha um json com as informações sobre a ultima liberação, essas informaçoes poderam ser vizualizadas no app para comprovar se o alimento realmente foi despejado
    ultimaLiberacao = "{\"dataUltimaLiberacao\":" + data + ",\"quantidadeSolicitada\":\"" + (String)gramas + "\",\"quantidadeLiberada\":\"" + (String)balanca.get_units(2) + "\",\"status\":\"" + status + "\"}";
    //grava na eeprom essas informações sobre a ultima liberação
    gravaEeprom();

    //envia dados dos sensores
    sendSensores();



  }

}

void botaoPressionado() {
  statusBotaoLiberaRacao = true;
}

void checaSeEhoraDeLiberarRacao() {

  date.begin("date");
  date.addParameter("+%H:%M");
  date.run();
  while (date.available() > 0) {
    //leia a data atual
    String horarioAtual = date.readStringUntil('\n');
    //String hora = (horarioAtual.substring(0, horarioAtual.indexOf(':')));
    //String minuto =(horarioAtual.substring(horarioAtual.indexOf(':')+1, (horarioAtual.length()+1) ));
    //Console.println("Data : " + (String)horarioAtual);
    //Console.println("Hora extraida: " + (String)hora);
    //Console.println("minuto extraido: " + (String)minuto);
    //o ideal seria se o tamanho do StaticJsonDocument fosse definido pelo tamanho da string agendamento...
    StaticJsonDocument<350> jsonAgendamento;

    //descubra o tamanho da string agendamento e adiciona 1
    int agendamento_len = agendamento.length() + 1;
    //cria uma array de char do tamanho da string agendamento+1
    char agendamentoCharArray[agendamento_len];
    //converte a string agendamento para o array de char agendamentoCharArray
    agendamento.toCharArray(agendamentoCharArray, agendamento_len);

    // desserializar JSON contido dentro da string agendamento, esse json armazenado nessa variavel tem esse formato [{"horario":"12:00","quantidadegramas":150},{"horario":"08:00","quantidadegramas":25},{"horario":"18:00","quantidadegramas":30}]
    DeserializationError error = deserializeJson(jsonAgendamento, agendamentoCharArray);
    //verifica se conseguiu converter o json que estava dentro de "agendamentoCharArray"
    if (error) {
      //se nao conseguiu imprime erro ao desserializar o json
      Console.print(F("falha ao deserializar o agendamento: "));
      Console.println(error.f_str());
      Console.flush();

    } else {
      for (JsonObject elem : jsonAgendamento.as<JsonArray>()) {


        String horarioAgendado = elem["horario"];
        int quantidadeAgendada = elem["quantidadegramas"].as<unsigned int>();

        //aqui esta sendo comparada a horaAtual com uma a horaAgendamento que esta presente no JsonArray de agendamento, estou fazendo substring dos campos que representam a hora e dos campos que representam o minuto
        if (horarioAtual.substring(0, horarioAtual.indexOf(':')).compareTo(horarioAgendado.substring(0, horarioAgendado.indexOf(':'))) == 0 && horarioAtual.substring(horarioAtual.indexOf(':') + 1, (horarioAtual.length() + 1)).compareTo(horarioAgendado.substring(horarioAgendado.indexOf(':') + 1, horarioAgendado.length() + 1)) == 0) {
          liberaRacao(quantidadeAgendada);
        }

        Console.print("Horario: " + horarioAgendado);
        Console.println("  Quantidade: " + String(quantidadeAgendada));
        Console.flush();
      }
    }
  }

}

void gravaEeprom() {

  String stringAserGravada = agendamento + "&" + ultimaLiberacao + "&" + TO_EMAIL_ADDRESS;
  int tamanhoDaString = stringAserGravada.length();

  //Console.println("O que será gravado na eeprom é: " + stringAserGravada);
  //Console.flush();

  //os primeiros dois bytes (endereço 0, 1) da eeprom são utilizados para informar quantos bytes devem ser lidos, ou seja o tamanho da string
  EEPROM.write(0, tamanhoDaString >> 8);
  EEPROM.write(1, tamanhoDaString & 0xFF);

  //percorra o tamanhado da string e grave cada caractere na eeprom apartir do endereço de memoria na posicao 2...
  for (int i = 0; i < tamanhoDaString; i++) {
    EEPROM.write(i + 2, stringAserGravada[i]);
  }


}

void recuperaEeprom() {

  //aqui recuperamos a quantidade de bytes para ler da eeprom, ou seja qual o tamanho da string armazenada na eeprom, lembrando que a string está realmente armazenada apartir do endereço de memoria 2, pois o 0 e 1 formar a quantidade de bytes a serem lidos
  unsigned int qtdeDeBytesParaLer =  ((EEPROM.read(0) << 8) + EEPROM.read(1));

  //por padrao a primeira inicialização do arduino após um upload de esboço, coloca toda a memoria eeprom preenchida com 0xFF, 0xFF, 0xFF.....
  //dois bytes seguidos com 0xFF formam o numero 65535. Logo se NAO for esse numero, temos algo valido gravado na eeprom e podemos tentar recuperar a informação
  if (qtdeDeBytesParaLer != 65535) {
    String stringRecuperadaDaEeprom = "";

    for (int i = 0; i < qtdeDeBytesParaLer; i++) {
      stringRecuperadaDaEeprom += (char)EEPROM.read(i + 2);
    }

    agendamento = stringRecuperadaDaEeprom.substring(0, stringRecuperadaDaEeprom.indexOf('&'));
    ultimaLiberacao = stringRecuperadaDaEeprom.substring(stringRecuperadaDaEeprom.indexOf('&') + 1, (stringRecuperadaDaEeprom.lastIndexOf('&')));
    TO_EMAIL_ADDRESS = stringRecuperadaDaEeprom.substring(stringRecuperadaDaEeprom.lastIndexOf('&') + 1, (stringRecuperadaDaEeprom.length() + 1));
    //Console.println("O agendamento Recuperado da eeprom é:" + agendamento + " A ultima liberação recuperada da eeprom é: " + ultimaLiberacao+ " O email recuperado é: "+TO_EMAIL_ADDRESS);
    //Console.flush();

  }


}

void sendMail(String message) {

  //Console.println("Running SendAnEmail...");

  //checa se o email esta ou nao definido para nulo. Lembrando que se definido para nulo significa que não queremos enviar emails de feedback
  if (!TO_EMAIL_ADDRESS.equals("nulo")) {
    TembooYunShieldChoreo  SendEmailChoreo;

    // invoke the Temboo client
    // NOTE that the client must be reinvoked, and repopulated with
    // appropriate arguments, each time its run() method is called.
    SendEmailChoreo.begin();

    // set Temboo account credentials
    SendEmailChoreo.setAccountName(TEMBOO_ACCOUNT);
    SendEmailChoreo.setAppKeyName(TEMBOO_APP_KEY_NAME);
    SendEmailChoreo.setAppKey(TEMBOO_APP_KEY);

    // identify the Temboo Library choreo to run (Google > Gmail > SendEmail)
    SendEmailChoreo.setChoreo("/Library/Google/Gmail/SendEmail");


    // set the required choreo inputs
    // see https://www.temboo.com/library/Library/Google/Gmail/SendEmail/
    // for complete details about the inputs for this Choreo

    // the first input is your Gmail email address
    SendEmailChoreo.addInput("Username", GMAIL_USER_NAME);
    // next is your Gmail App-Specific password.
    SendEmailChoreo.addInput("Password", GMAIL_PASSWORD);
    // who to send the email to
    SendEmailChoreo.addInput("ToAddress", TO_EMAIL_ADDRESS);
    // then a subject line
    SendEmailChoreo.addInput("Subject", "ALERT: AutoPetfeeder");

    // next comes the message body, the main content of the email
    SendEmailChoreo.addInput("MessageBody", message);

    // tell the Choreo to run and wait for the results. The
    // return code (returnCode) will tell us whether the Temboo client
    // was able to send our request to the Temboo servers
    unsigned int returnCode = SendEmailChoreo.run();

    // a return code of zero (0) means everything worked
    if (returnCode == 0) {
      Console.println(F("Success! Email sent!"));
    } else {
      Console.println(F("Falha ao enviar email"));
      // a non-zero return code means there was an error
      // read and print the error message
      while (SendEmailChoreo.available()) {
        char c = SendEmailChoreo.read();
        //Console.print(c);
      }
    }
    SendEmailChoreo.close();
  } else {
    //Console.println("Email setado como nulo");
    //Console.flush();
  }

}
