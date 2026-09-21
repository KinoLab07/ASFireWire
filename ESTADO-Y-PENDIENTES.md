# Estado y pendientes — 21 de septiembre de 2026

Sustituye a la sección 10 de `MBOX_PRO_PANEL.md`, que queda como material del
artículo pero ya no como lista de trabajo.

---

## Qué funciona, medido

| Función | Estado |
|---|---|
| 6 salidas de línea | funciona |
| S/PDIF | enrutado |
| Auriculares A y B | funcionan |
| 4 entradas XLR | funcionan |
| **Todo simultáneo** | **sí** |
| Medidores del panel frontal | funcionan (muestran las entradas) |
| LED del panel | controlable por registro |
| Mezclador desde el host | escribe y responde, verificado |

Versión instalada y funcionando: **0.3.0 build 15**.

La cadena de control se demostró de punta a punta: silenciar las dos tiras de
playback lleva el altavoz a **0 exacto** y restaurarlas lo devuelve. La ley de
paneo de potencia constante se midió en **+3,2 dB** de centro a extremo
(1799 / 1241 / 15).

---

## Decisión: el panel de control gráfico queda abandonado

No compensa. Los pendientes de interfaz están **cancelados**, no aplazados:
etiquetar pestañas, medidor de máster, botón `Mono`, `Left Trim`, bloque de
efectos, sección de máster, botones S/M, las capturas `shot-2`…`shot-7` y la
sustitución del arte de Avid por diseños propios.

Lo que **sí** se queda, porque es conocimiento del aparato y no de la interfaz:

- La orientación del router: **`(origen << 8) | destino`**. Byte alto origen,
  byte bajo destino. Estuvo documentado al revés durante días.
- La geometría del mezclador: 18 entradas × 16 salidas, índice
  `salida*18 + entrada`, unidad `0x4000`, coeficientes desde el offset +4.
- La sección de picos **no tiene cabecera de cuenta**: la entrada 0 está en
  `PEAK+0`, no en `PEAK+4`. Formato `(nivel << 16) | entrada_de_router`.
- El mapa pestaña → destino físico, y que las salidas **3-4 son un duplicado de
  las 1-2** porque ambas se alimentan de `MIXo:0/1`.
- La matemática de Avid (paneo, fader, balance), recuperada de `MixerUtils` y
  verificada contra el aparato. Está en `MBOX_PRO_PANEL.md`, sección 4.

---

## Limpieza antes de publicar

Cuatro pasos. Ninguno toca el driver.

**1. Borrar los tres archivos del panel:**

```bash
cd <repo>
rm ASFW/Views/MboxProControlView.swift
rm ASFW/ViewModels/MboxProControlViewModel.swift
rm ASFW/Views/Components/MboxPanelComponents.swift
```

**2. Desconectarlo de la navegación.** En `ASFW/Views/ModernContentView.swift`
hay exactamente cuatro referencias:

| Línea | Qué es |
|---|---|
| 59 | `case mboxPro = "Mbox Pro"` — la entrada del enum |
| 85 | el icono `"hifispeaker.and.homepod.fill"` |
| 145 | `case .mboxPro:` |
| 146 | `MboxProControlView(connector: debugVM.connector)` |

Quita las cuatro. No hay más usos en todo el árbol.

**3. Sacar el arte de Avid:**

```bash
rm -rf ASFW/Resources/MboxPro
```

Ya está en `.gitignore`, así que nunca llegó a git — pero sin el panel tampoco
tiene sentido que siga en el disco de trabajo. La copia de referencia se queda
en `2-REFERENCIA-AVID/`. **Deja la entrada del `.gitignore` puesta**: no estorba
y protege de un descuido futuro.

**4. Regenerar y compilar:**

```bash
xcodegen generate && ./build.sh
```

El target `ASFW` toma `sources: - path: ASFW`, la carpeta entera, así que borrar
archivos basta: no hay que tocar `project.yml`.

---

## Pendientes vivos

### Driver

- [ ] **Validar en frío el arranque.** Que tras encender desde cero el aparato
      salga programado y sonando sin intervención. Se arregló la lógica el 21 de
      septiembre pero **no se ha comprobado desde un arranque limpio**.
- [ ] **Mover la programación del router al enganche del dispositivo**, no a la
      puesta en marcha del audio. Hoy el router lee 0 entradas hasta que arranca
      el audio, lo que confunde cualquier diagnóstico.
- [ ] **Confirmar el fondo de escala del medidor.** Se asume `4095` por
      observación — nunca se ha visto un valor mayor — pero sin señal de
      referencia que lo confirme.
- [ ] **Investigar los timeouts del camino asíncrono.** Tras instalar la v15
      aparecieron ~200 mensajes de *"el aparato confirma pero responde tarde"* y
      el control acabó colgado, mientras el audio isócrono seguía sano.
      Desenchufar y enchufar lo recuperó; reiniciar lo arregló del todo.
      **No está establecido si lo provocó el cambio de la v15.** Si vuelve a
      pasar, la forma de aislarlo es volver a la v14 y ver si desaparece.
- [ ] **Llevar aguas arriba** el arreglo del buzón de notificaciones (solo
      admitía un observador) y el perfil de la Mbox Pro.

### Preguntas abiertas

- [ ] Qué es el **bloque 3** como fuente. Es real y alimenta el auricular A, pero
      no se sabe qué produce.
- [ ] Por qué los canales **1 y 2** marcan nivel sin nada conectado.

### Operativa

- [x] **`git commit`** — hecho el 21 de septiembre. Cinco commits locales sobre
      `main`: el buzón de notificaciones, el arreglo de CLOCK_SELECT, el aparato
      entero (router, mezclador, picos), los tres documentos y el `deploy.sh` con
      el número de versión. El panel quedó fuera sin borrarse, así que lo
      commiteado compila sin él.
- [ ] **Empujar a `origin/main`** — a propósito todavía no. Primero instalar
      Tahoe en esta máquina y comprobar que el aparato sigue sonando. Y antes de
      empujar, la limpieza del panel de aquí arriba.
- [ ] Antes de instalar, **siempre** `systemextensionsctl list`. Dos entradas →
      reiniciar antes de intentarlo.

---

## Lo que ya se decidió y no hay que volver a investigar

- **Ejecutar el Panel original de Avid en Apple Silicon es imposible.** Tres
  bloqueos independientes y verificados: la CPU es arm64 y Rosetta traduce
  x86_64, no i386; macOS retiró los 32 bits en Catalina; `libstdc++.6.dylib` ya
  no existe y Carbon, AGL y OpenGL son cascarones. La única vía real sería un Mac
  Intel con Mojave.
- **El aparato no está limitado a un banco de salida a la vez.** Durante dos días
  se concluyó eso y se formuló una regla —"gana el puerto con más referencias
  como fuente"— que encajaba con siete experimentos y era falsa. Describía el
  defecto de nuestras propias tablas, que saltaban el mezclador.
- **Sin coeficientes de mezclador no suena nada**, porque las salidas se
  alimentan de `MIXo`. La tabla de rutas sola es media configuración.
- **Instalar en una máquina con la seguridad íntegra no es posible hoy**, y no
  por falta de un clic: los permisos de DriverKit restringidos no tienen diálogo
  de excepción. Hace falta la concesión de Apple + Developer ID + notarización.

---

## La lección de método

Lo que hizo avanzar el trabajo fue, invariablemente, **leer el aparato en vez de
razonar sobre él**. El observador que vigilaba las celdas del mezclador resolvió
en veinte segundos lo que tres rondas de parches a ciegas no habían aclarado: no
llegaba ni una escritura.

Y lo que lo frenó fue **dar por buenas mediciones sin verificar el instrumento**.
Una prueba entera del mezclador se dio por concluyente cuando el script mandaba
el campo equivocado y no escribía nada; la conclusión que se sacó de ella era
falsa y apuntaba en dirección contraria a la verdad.
