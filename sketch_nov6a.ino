#include <Adafruit_GFX.h>
#include <Adafruit_PCD8544.h>
#include <EEPROM.h>
#include <stdio.h>
#include <string.h>

// ==================================================
// 1. PINES
// ==================================================

#define ledVerde 2
#define ledAmarillo 3
#define ledRojo 4
#define ledBlanco 5

#define Trigger 7
#define Echo 6

#define Buzzer 11

// Pantalla Nokia 5110: CLK, DIN, DC, CE, RST
Adafruit_PCD8544 pantalla(13, 8, A1, A0, 12);

// Encoder
#define encoderCLK A2
#define encoderDT A3
#define encoderSW A4

const int ANCHO_PANTALLA = 84;
const int ALTO_PANTALLA = 48;

// 2. VARIABLES

const float sonido = 34300.0;

// Correccion en centimetros tras comparar con una regla
const float CORRECCION_CM = 0.0;

// Filtro del sensor
const int NUM_MUESTRAS = 30;
const unsigned long TIEMPO_MAXIMO = 30000UL;

// Histéresis: evita cambios por variaciones pequenas
const float HIST_VERDE = 3.0;
const float HIST_ROJO = 2.0;

bool maniobraActiva = false;
bool resumenDisponible = false;

float distancia = -1;
float distanciaMinima = 999.0;
float ultimaMinimaGuardada = -1;

unsigned long tiempoInicio = 0;
unsigned long tiempoFinal = 0;
unsigned long tiempoAnteriorPantalla = 0;
unsigned long ultimoCambioBoton = 0;

int modo = 0;
int ultimoCLK = HIGH;
int tonoActual = 0;

bool estadoBotonAnterior = HIGH;

const char* nombresModos[] = {
  "NORMAL",
  "SILENCIOSO",
  "PRECISION"
};

// Zonas de proximidad
enum Zona {
  ZONA_BLANCA,
  ZONA_VERDE,
  ZONA_AMARILLA,
  ZONA_ROJA
};

Zona zonaActual = ZONA_BLANCA;

// EEPROM
const int direccionMinima = 0;
const int direccionMarca = 10;
const byte marcaValida = 123;

// ==================================================
// 3. TEXTO CENTRADO
// ==================================================

void textoCentrado(const char* texto, int y, int tamano) {
  pantalla.setTextSize(tamano);

  int anchoTexto = strlen(texto) * 6 * tamano;
  int x = (ANCHO_PANTALLA - anchoTexto) / 2;

  if (x < 0) x = 0;

  pantalla.setCursor(x, y);
  pantalla.print(texto);
}


// 4. APAGAR ALERTAS


void apagarAlertas() {
  digitalWrite(ledVerde, LOW);
  digitalWrite(ledAmarillo, LOW);
  digitalWrite(ledRojo, LOW);

  if (tonoActual != 0) {
    noTone(Buzzer);
    tonoActual = 0;
  }
}

// 5. MEDIR UNA VEZ


float medirDistancia() {
  digitalWrite(Trigger, LOW);
  delayMicroseconds(2);

  digitalWrite(Trigger, HIGH);
  delayMicroseconds(10);
  digitalWrite(Trigger, LOW);

  unsigned long tiempo =
    pulseIn(Echo, HIGH, TIEMPO_MAXIMO);

  if (tiempo == 0) {
    return -1.0;
  }

  return tiempo * sonido / 2000000.0;
}


// 6. FILTRO: MEDIANA DE TRES MUESTRAS


float medirDistanciaFiltrada() {
  float muestras[NUM_MUESTRAS];
  int validas = 0;

  Serial.println();
  Serial.println("--- MEDICIONES DEL SENSOR ---");

  for (int i = 0; i < NUM_MUESTRAS; i++) {
    float muestra = medirDistancia();

    Serial.print("Muestra ");
    Serial.print(i + 1);
    Serial.print(": ");

    if (muestra >= 2.0 && muestra <= 400.0) {
      muestras[validas] = muestra;
      validas++;

      Serial.print(muestra, 1);
      Serial.println(" cm");
    } else {
      Serial.println("SIN ECO / LECTURA INVALIDA");
    }

    // Separacion entre pulsos para evitar ecos anteriores
    delay(40);
  }

  // Exigir al menos tres mediciones validas
  if (validas < 3) {
    Serial.println("Resultado: SIN ECO VALIDO");
    return -1.0;
  }

  // Ordenar las muestras
  for (int i = 0; i < validas - 1; i++) {
    for (int j = 0; j < validas - i - 1; j++) {
      if (muestras[j] > muestras[j + 1]) {
        float auxiliar = muestras[j];
        muestras[j] = muestras[j + 1];
        muestras[j + 1] = auxiliar;
      }
    }
  }

  // Calcular mediana
  float mediana;

  if (validas % 2 == 1) {
    mediana = muestras[validas / 2];
  } else {
    mediana = (
      muestras[validas / 2 - 1] +
      muestras[validas / 2]
    ) / 2.0;
  }

  float corregida = mediana + CORRECCION_CM;

  if (corregida < 0) corregida = 0;

  Serial.print("Muestras validas: ");
  Serial.print(validas);
  Serial.print("/");
  Serial.println(NUM_MUESTRAS);

  Serial.print("MEDIANA: ");
  Serial.print(mediana, 1);
  Serial.println(" cm");

  Serial.print("DISTANCIA CORREGIDA: ");
  Serial.print(corregida, 1);
  Serial.println(" cm");

  return corregida;
}


// 7. UMBRALES POR MODO


// NORMAL y SILENCIOSO:
// Blanco: mas de 55 cm
// Verde: 30-55 cm
// Amarillo: 12-30 cm
// Rojo: 12 cm o menos
//
// PRECISION:
// Blanco: mas de 65 cm
// Verde: 35-65 cm
// Amarillo: 15-35 cm
// Rojo: 15 cm o menos

float limiteVerde() {
  return (modo == 2) ? 65.0 : 55.0;
}

float limiteAmarillo() {
  return (modo == 2) ? 35.0 : 30.0;
}

float limiteRojo() {
  return (modo == 2) ? 15.0 : 12.0;
}


// 8. ACTUALIZAR ZONA CON HISTÉRESIS


void actualizarZona(float d) {
  float lejos = limiteVerde();
  float medio = limiteAmarillo();
  float cerca = limiteRojo();

  // Sin lectura valida: zona blanca
  if (d < 0) {
    zonaActual = ZONA_BLANCA;
    return;
  }

  // Volver al blanco al alejarse claramente
  if (d >= lejos + HIST_VERDE) {
    zonaActual = ZONA_BLANCA;
    return;
  }

  switch (zonaActual) {

    case ZONA_BLANCA:
      // Entra en la zona correspondiente al acercarse
      if (d <= cerca - HIST_ROJO) {
        zonaActual = ZONA_ROJA;
      }
      else if (d <= medio - HIST_VERDE) {
        zonaActual = ZONA_AMARILLA;
      }
      else if (d <= lejos - HIST_VERDE) {
        zonaActual = ZONA_VERDE;
      }
      break;

    case ZONA_VERDE:
      // El verde no cambia al amarillo por variaciones pequenas
      if (d <= cerca - HIST_ROJO) {
        zonaActual = ZONA_ROJA;
      }
      else if (d <= medio - HIST_VERDE) {
        zonaActual = ZONA_AMARILLA;
      }
      break;

    case ZONA_AMARILLA:
      if (d <= cerca - HIST_ROJO) {
        zonaActual = ZONA_ROJA;
      }
      else if (d >= medio + HIST_VERDE) {
        zonaActual = ZONA_VERDE;
      }
      break;

    case ZONA_ROJA:
      // Si se aleja bastante, puede salir directamente al verde
      if (d >= medio + HIST_VERDE) {
        zonaActual = ZONA_VERDE;
      }
      else if (d >= cerca + HIST_ROJO) {
        zonaActual = ZONA_AMARILLA;
      }
      break;
  }
}


// 9. MEMORIA EEPROM


void guardarUltimaMinima() {
  if (distanciaMinima < 999.0) {
    int valor =
      (int)(distanciaMinima * 10.0 + 0.5);

    EEPROM.put(direccionMinima, valor);
    EEPROM.update(direccionMarca, marcaValida);

    ultimaMinimaGuardada = valor / 10.0;
  }
}

void leerUltimaMinima() {
  if (EEPROM.read(direccionMarca) == marcaValida) {
    int valor;
    EEPROM.get(direccionMinima, valor);

    if (valor >= 0 && valor <= 4000) {
      ultimaMinimaGuardada = valor / 10.0;
    }
  }
}

// ==================================================
// 10. MENU PRINCIPAL
// ==================================================

void mostrarMenu() {
  pantalla.clearDisplay();
  pantalla.setTextSize(1);
  pantalla.setTextColor(BLACK);

  pantalla.setCursor(0, 0);
  pantalla.print("PARKING SMART");
  pantalla.drawLine(0, 9, 83, 9, BLACK);

  pantalla.setCursor(0, 12);
  pantalla.print("MODO");

  pantalla.setCursor(24, 12);
  pantalla.print(nombresModos[modo]);

  pantalla.setCursor(0, 22);
  pantalla.print("Gira: elegir");

  pantalla.setCursor(0, 31);
  pantalla.print("Pulsa: iniciar");

  pantalla.setCursor(0, 40);
  pantalla.print("ULT: ");

  if (ultimaMinimaGuardada >= 0) {
    pantalla.print(ultimaMinimaGuardada, 0);
    pantalla.print("cm");
  } else {
    pantalla.print("--");
  }

  pantalla.display();
}

// ==================================================
// 11. BARRA GRAFICA DE PROXIMIDAD
// ==================================================

void dibujarBarra() {
  const int x = 2;
  const int y = 38;
  const int ancho = 80;
  const int alto = 8;

  pantalla.drawRect(x, y, ancho, alto, BLACK);

  if (distancia < 0) {
    return;
  }

  float proporcion = 1.0 - distancia / limiteVerde();

  if (proporcion < 0) proporcion = 0;
  if (proporcion > 1) proporcion = 1;

  int relleno = (int)(proporcion * (ancho - 2));

  if (relleno > 0) {
    pantalla.fillRect(
      x + 1,
      y + 1,
      relleno,
      alto - 2,
      BLACK
    );
  }
}

// ==================================================
// 12. PANTALLA DURANTE EL APARCAMIENTO
// ==================================================

void mostrarManiobra() {
  pantalla.clearDisplay();
  pantalla.setTextColor(BLACK);
  pantalla.setTextSize(1);

  pantalla.setCursor(0, 0);
  pantalla.print("APARCANDO ");

  if (modo == 0) pantalla.print("N");
  if (modo == 1) pantalla.print("S");
  if (modo == 2) pantalla.print("P");

  pantalla.drawLine(0, 9, 83, 9, BLACK);

  char textoDistancia[12];

  if (distancia < 0) {
    strcpy(textoDistancia, "SIN ECO");
  } else {
    int cm = (int)(distancia + 0.5);

    if (cm < 100) {
      snprintf(
        textoDistancia,
        sizeof(textoDistancia),
        "%d cm",
        cm
      );
    } else {
      snprintf(
        textoDistancia,
        sizeof(textoDistancia),
        "%dcm",
        cm
      );
    }
  }

  textoCentrado(textoDistancia, 11, 2);

  char lineaDatos[24];

  unsigned long segundos =
    (millis() - tiempoInicio) / 1000UL;

  unsigned long minutos = (segundos / 60UL) % 100UL;
  unsigned long seg = segundos % 60UL;

  if (distanciaMinima < 999.0) {
    int minimo = (int)(distanciaMinima + 0.5);

    snprintf(
      lineaDatos,
      sizeof(lineaDatos),
      "M:%dcm %02lu:%02lu",
      minimo,
      minutos,
      seg
    );
  } else {
    snprintf(
      lineaDatos,
      sizeof(lineaDatos),
      "M:--cm %02lu:%02lu",
      minutos,
      seg
    );
  }

  pantalla.setTextSize(1);
  pantalla.setCursor(0, 28);
  pantalla.print(lineaDatos);

  dibujarBarra();
  pantalla.display();
}

// ==================================================
// 13. VALORACION FINAL
// ==================================================

const char* evaluarManiobra() {
  if (distanciaMinima >= 999.0) {
    return "SIN DATOS";
  }

  if (distanciaMinima < 10.0) {
    return "PELIGRO";
  }

  if (distanciaMinima < 30.0) {
    return "MUY CERCA";
  }

  if (distanciaMinima <= 50.0) {
    return "BUENA";
  }

  return "LEJOS";
}

// ==================================================
// 14. PANTALLA DE RESULTADOS
// ==================================================

void mostrarResumen() {
  pantalla.clearDisplay();
  pantalla.setTextColor(BLACK);
  pantalla.setTextSize(1);

  pantalla.setCursor(0, 0);
  pantalla.print("RESULTADO");
  pantalla.drawLine(0, 9, 83, 9, BLACK);

  char textoMinimo[12];

  if (distanciaMinima < 999.0) {
    int minimo = (int)(distanciaMinima + 0.5);

    if (minimo < 100) {
      snprintf(
        textoMinimo,
        sizeof(textoMinimo),
        "%d cm",
        minimo
      );
    } else {
      snprintf(
        textoMinimo,
        sizeof(textoMinimo),
        "%dcm",
        minimo
      );
    }
  } else {
    strcpy(textoMinimo, "-- cm");
  }

  textoCentrado(textoMinimo, 11, 2);

  unsigned long segundos =
    (tiempoFinal - tiempoInicio) / 1000UL;

  unsigned long minutos = (segundos / 60UL) % 100UL;
  unsigned long seg = segundos % 60UL;

  pantalla.setTextSize(1);
  pantalla.setCursor(0, 29);
  pantalla.print("Tiempo: ");

  if (minutos < 10) pantalla.print("0");
  pantalla.print(minutos);
  pantalla.print(":");

  if (seg < 10) pantalla.print("0");
  pantalla.print(seg);

  pantalla.setCursor(0, 39);
  pantalla.print("VAL: ");
  pantalla.print(evaluarManiobra());

  pantalla.display();
}

// ==================================================
// 15. LEDS Y ZUMBADOR ESTABILIZADOS
// ==================================================

void actualizarAlertas() {
  actualizarZona(distancia);

  digitalWrite(ledVerde, LOW);
  digitalWrite(ledAmarillo, LOW);
  digitalWrite(ledRojo, LOW);

  int nuevaFrecuencia = 0;

  if (zonaActual == ZONA_BLANCA) {
    digitalWrite(ledBlanco, HIGH);
  }
  else {
    digitalWrite(ledBlanco, LOW);

    if (zonaActual == ZONA_VERDE) {
      digitalWrite(ledVerde, HIGH);
      if (modo != 1) nuevaFrecuencia = 2000;
    }
    else if (zonaActual == ZONA_AMARILLA) {
      digitalWrite(ledAmarillo, HIGH);
      if (modo != 1) nuevaFrecuencia = 2500;
    }
    else if (zonaActual == ZONA_ROJA) {
      digitalWrite(ledRojo, HIGH);
      if (modo != 1) nuevaFrecuencia = 3000;
    }
  }

  // Solo cambiar el tono cuando cambie la zona
  if (nuevaFrecuencia != tonoActual) {
    if (nuevaFrecuencia == 0) {
      noTone(Buzzer);
    } else {
      tone(Buzzer, nuevaFrecuencia);
    }

    tonoActual = nuevaFrecuencia;
  }
}

// ==================================================
// 16. LEER GIRO DEL ENCODER
// ==================================================

void leerEncoder() {
  int estadoCLK = digitalRead(encoderCLK);

  if (estadoCLK != ultimoCLK &&
      estadoCLK == LOW) {

    if (!maniobraActiva && !resumenDisponible) {
      if (digitalRead(encoderDT) != estadoCLK) {
        modo++;
      } else {
        modo--;
      }

      if (modo > 2) modo = 0;
      if (modo < 0) modo = 2;

      mostrarMenu();
    }
  }

  ultimoCLK = estadoCLK;
}

// ==================================================
// 17. DETECTAR PULSACION DEL ENCODER
// ==================================================

bool encoderPulsado() {
  bool estadoActual = digitalRead(encoderSW);

  if (estadoActual == LOW &&
      estadoBotonAnterior == HIGH &&
      millis() - ultimoCambioBoton > 250) {

    ultimoCambioBoton = millis();
    estadoBotonAnterior = estadoActual;

    return true;
  }

  estadoBotonAnterior = estadoActual;
  return false;
}

// ==================================================
// 18. INICIAR MANIOBRA
// El sensor solo mide desde este momento
// ==================================================

void iniciarManiobra() {
  maniobraActiva = true;
  resumenDisponible = false;

  distancia = -1;
  distanciaMinima = 999.0;

  tiempoInicio = millis();
  tiempoAnteriorPantalla = 0;

  // Reiniciar la histéresis al comenzar
  zonaActual = ZONA_BLANCA;

  apagarAlertas();
  digitalWrite(ledBlanco, LOW);

  mostrarManiobra();
}

// ==================================================
// 19. FINALIZAR MANIOBRA
// ==================================================

void finalizarManiobra() {
  maniobraActiva = false;
  resumenDisponible = true;

  tiempoFinal = millis();

  apagarAlertas();

  // El sensor ya no esta activo
  digitalWrite(ledBlanco, LOW);

  guardarUltimaMinima();
  mostrarResumen();
}

// ==================================================
// 20. CONFIGURACION INICIAL
// ==================================================

void setup() {
  pinMode(ledVerde, OUTPUT);
  pinMode(ledAmarillo, OUTPUT);
  pinMode(ledRojo, OUTPUT);
  pinMode(ledBlanco, OUTPUT);

  pinMode(Trigger, OUTPUT);
  pinMode(Echo, INPUT);
  pinMode(Buzzer, OUTPUT);

  pinMode(encoderCLK, INPUT_PULLUP);
  pinMode(encoderDT, INPUT_PULLUP);
  pinMode(encoderSW, INPUT_PULLUP);

  digitalWrite(Trigger, LOW);

  apagarAlertas();
  digitalWrite(ledBlanco, LOW);

  Serial.begin(9600);

  pantalla.begin();
  pantalla.setContrast(40);
  pantalla.clearDisplay();
  pantalla.display();

  leerUltimaMinima();

  ultimoCLK = digitalRead(encoderCLK);

  // Sensor inactivo mientras se selecciona el modo
  maniobraActiva = false;
  resumenDisponible = false;
  zonaActual = ZONA_BLANCA;

  mostrarMenu();
}

// ==================================================
// 21. BUCLE PRINCIPAL
// ==================================================

void loop() {
  leerEncoder();

  if (encoderPulsado()) {

    // Primer toque: iniciar
    if (!maniobraActiva && !resumenDisponible) {
      iniciarManiobra();
    }

    // Segundo toque: finalizar
    else if (maniobraActiva) {
      finalizarManiobra();
    }

    // Tercer toque: regresar al menu
    else {
      resumenDisponible = false;

      apagarAlertas();
      digitalWrite(ledBlanco, LOW);

      mostrarMenu();
    }
  }

  // El sensor solamente mide durante la maniobra
  if (maniobraActiva) {
    distancia = medirDistanciaFiltrada();

    if (distancia >= 0 &&
        distancia < distanciaMinima) {
      distanciaMinima = distancia;
    }

    actualizarAlertas();

    if (millis() - tiempoAnteriorPantalla >= 200) {
      tiempoAnteriorPantalla = millis();
      mostrarManiobra();
    }
  }

  delay(10);
}
