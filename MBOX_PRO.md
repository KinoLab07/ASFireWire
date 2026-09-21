# Avid Mbox Pro en macOS moderno

Interfaz FireWire de 2011 (DICE II / TCAT), funcionando completa en un MacBook
Pro M4 Pro con macOS Tahoe a través de ASFireWire. El driver de Avid se detiene
en Mojave y Apple retiró la pila FireWire del sistema.

## Estado

| Función | Estado |
|---|---|
| 6 salidas de línea | funciona |
| S/PDIF | enrutado |
| Auriculares A y B | funcionan |
| 4 entradas XLR | funcionan |
| **Todo simultáneo** | **sí** |
| Medidores del panel frontal | funcionan (muestran las entradas) |
| LED del panel | controlable por registro |

El auricular A monitoriza las salidas 1-2 y el B las 3-4, que es el
comportamiento de fábrica documentado por Avid.

## La topología: todo pasa por el mezclador

Esta es la clave de todo, y costó dos días entenderla porque intentamos
deducirla en vez de mirarla.

```
playback (ATX0) ─┬─> InS0:0-5        (directo)
                 └─> AES:0-1

entradas analógicas (InS0 fuente) ─> MIX0:0-1,4-5 ┐
S/PDIF in (AES fuente) ───────────> MIX0:6-7      ├─> mezclador
InS1 fuente ──────────────────────> MIX0:8-13     ┘
                                                   │
                     MIXo:0-5 ──────────────> InS0:0-5   salidas de línea
                     MIXo:6-7 ──────────────> AES:0-1    S/PDIF
                     MIXo:8-15 ─────────────> ARX0:0-7   captura al host
                     blk3:0-1 ──────────────> InS1:0-1   auricular A
```

Las salidas de línea y S/PDIF aparecen **dos veces** como destino: desde
playback directo y desde el mezclador. Avid programa ambas y el aparato lo
resuelve.

**Sin coeficientes de mezclador no suena nada**, porque las salidas se
alimentan de `MIXo`. La tabla de rutas sola es media configuración.

### Geometría del mezclador

18 entradas × 16 salidas, índice `salida*18 + entrada`, unidad `0x4000`,
coeficientes desde el offset +4 de la sección. Salidas 0-1 son la mezcla
estéreo, 2-7 pases directos, 8-15 envíos a la captura.

## Matemática del mezclador (extraída de `MixerUtils` en el Panel)

- **Paneo de potencia constante**, centro en `0.7071067811865475` (−3 dB), con
  zona muerta `0.4999000132083893 … 0.5039399862289429` para que enganche.
- **Fader**: suelo −75 dB, quiebre en −20 dB, recorrido repartido 0.625 arriba
  y 0.375 abajo. Implementado lineal en dB por tramo; el original curva
  suavemente dentro de cada tramo y esa forma exacta no se extrajo.

## Registros vendor (sección `application` de EAP)

Extraídos del `LaunchdDaemon` de Avid, que trae símbolos completos. El control
va por `LDiceExtCtrlRequestTarget`, que manda `(operación, espacio, offset,
tamaño)` al kext; el kext lo pasa crudo a FireWire. `espacio` es el índice de
sección EAP: 1=cmd, 2=mixer, 4=router, 7=standalone, 8=application.

| off | registro | notas |
|---|---|---|
| +0x00 | StandaloneMode | el campo son los bits 6-7 |
| +0x04 | UIButtonState | |
| +0x08 | *(estado, lo reescribe el firmware)* | |
| +0x0C | UITarget | máscara de pares que siguen la perilla maestra |
| +0x10 | DeviceMode | **comando**: el firmware lo consume y lo deja en 0 |
| +0x14 | UILEDState | |
| +0x18 | *(estado)* | |
| +0x1C | PWMOff | |
| +0x20 | ButtonHoldTime | |
| +0x24 | InputControl | filtro paso-alto |
| +0x28..0x3C | DAC1-DAC6 | **trim de las 6 salidas de línea** |
| +0x40..0x4C | EfxProgram / Volume / Time / Feedback | |

**Endianness**: el campo de estos registros es el **byte BAJO** de lo que
devuelve una lectura de quadlet normal. Prueba: DAC1 lee `0x1c` = 28 = trim de
−14 dB.

- `UILEDState`, byte de control: bits 6-5 = selección de monitor, 4 = AUX,
  2 = MONO. Selección 0 deja el LED apagado, 1 lo enciende verde.
- `DeviceMode`, byte de control: bit 0 = MUTE, bit 1 = DIM, bit 3 = TUNER.
- `DAC1-6`: `valor = clamp(-2 × dB, 0, 255)`, índice = `par*2 + canal`.

## API para el controlador

Genérica, en `IDeviceProtocol`, así que cualquier aparato la hereda:

```cpp
// Modos de salida (configuraciones de salida excluyentes)
uint32_t GetOutputModeCount() const;
uint32_t GetActiveOutputMode() const;
const char* GetOutputModeName(uint32_t index) const;
IOReturn SelectOutputMode(uint32_t index);

// Trim por salida (valor crudo del registro)
uint32_t GetOutputTrimCount() const;
uint8_t GetOutputTrim(uint32_t index) const;
IOReturn SetOutputTrim(uint32_t index, uint8_t value);
IOReturn RefreshOutputTrims();

// Mezclador
uint32_t GetMixerInputCount() const;
uint32_t GetMixerOutputCount() const;
uint16_t GetMixerUnityGain() const;
uint16_t GetMixerCoefficient(uint32_t out, uint32_t in) const;
IOReturn SetMixerCoefficient(uint32_t out, uint32_t in, uint16_t gain);
IOReturn RefreshMixer();
```

Selectores de user client: 64/65 modos, 66/67 trim, 68/69 mezclador.
Swift: `outputModes`, `selectOutputMode`, `outputTrims`, `setOutputTrim`,
`mixerRow`, `setMixerCoefficient`.

## Trampas que nos costaron tiempo

- **El enum de selectores está duplicado**: en `ASFWDriverUserClient.iig` y otra
  copia local en el `.cpp`. Hay que añadirlos en las dos.
- **`GetFirstScalarInput` devuelve `uint32_t`** y trunca los GUID de 64 bits.
  Usar `GetFirstScalarInput64`. Afectaba también a `StartAudioStreaming`.
- **No lanzar ráfagas de transacciones**: 28 escrituras de golpe y solo entran 2.
  Encadenarlas, cada una en la respuesta de la anterior.
- **El anillo de logs devuelve los más ANTIGUOS**. Filtrar por texto con
  `contains`, nunca pedir los últimos N.
- **El nodo cambia en cada reconexión.** Resolverlo consultando el bus.
- **Hay dos ASFW.app**: la de DerivedData y la de `/Applications`. Si la interfaz
  no refleja los cambios, comprobar cuál corre.
- La sección `peak` **no es fiable** para rutas.

## Ejecutar el Panel original: imposible en Apple Silicon

Tres bloqueos independientes, verificados: la CPU es arm64 y Rosetta traduce
x86_64, no i386; macOS retiró los 32 bits en Catalina; y `libstdc++.6.dylib` ya
no existe, con Carbon, AGL y OpenGL reducidos a cascarones. Las empresas
**recompilan desde el fuente**, no traducen binarios. Única vía real: un Mac
Intel con Mojave o anterior. **No volver a investigar esto.**

## Material de referencia

En `~/Developer/mbox-reference/` (fuera del repo, a propósito):

- `panel-atlas.json` — 628 piezas de la interfaz original con sus rectángulos
  exactos, extraídas de `Skins.xml`. Perillas de **66 fotogramas**.
- `contact-*.png` — hojas de contacto con cada pieza etiquetada.
- `panel-strings.txt` — 219 cadenas de la interfaz.
- `mojave-capture/` — volcados del aparato con el driver original funcionando.
- `mojave-tool/` — `mboxdump.c`, la utilidad que los produjo.
- `crop.swift`, `sheet.swift` — herramientas de recorte y contacto.

**Licencia**: el arte es de Avid, solo referencia visual. No meterlo en el repo
ni distribuirlo. El plan es generar gráficos propios equivalentes en función,
diseñados de cero — recolorear el original seguiría siendo obra derivada.

## Lo que aprendimos por el camino

Vale la pena dejarlo escrito porque el camino equivocado fue largo y es
instructivo.

Durante dos días concluimos que **el aparato solo podía mantener un banco de
salida activo a la vez**, y se formuló una regla —"gana el puerto con más
referencias como fuente"— que encajaba con siete experimentos. Era falsa.
Describía el comportamiento de nuestras propias tablas, todas con el mismo
defecto de fondo: enrutaban el playback directo a las salidas y la captura
directa desde los puertos serie, **saltándose el mezclador**.

También se descartó por escrito el bloque fuente `blk3` como "salida de
mezclador inexistente, mute deliberado". Es real y es justo lo que alimenta el
auricular A.

Y se dio por nula la vía del mezclador tras una prueba hecha **sin coeficientes
cargados**, que por tanto no probaba nada.

Lo que lo resolvió no fue más análisis: fue **instalar el driver original en un
Mac con Mojave y leer qué tenía programado el aparato**. La configuración
correcta apareció en un volcado, y replicarla funcionó a la primera.

La lección: cuando existe una implementación de referencia que funciona,
medirla vale más que deducirla.

## Pendiente

- Interfaz: acercarla al original. Falta la captura del panel como PNG para
  medir proporciones.
- Medidores reales (sección `peak`) y bloque de efectos.
- Entender qué es `blk3` como fuente y por qué las entradas 8-13 del mezclador
  vienen de `InS1:0-5`. Sospecha: `InS1` en dirección fuente es el retorno del
  playback, los "Software Returns" del panel de Avid.
- Río arriba: el arreglo del buzón de notificaciones (solo admitía un
  observador) y el perfil del Mbox Pro.
