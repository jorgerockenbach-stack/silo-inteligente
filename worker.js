
export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    const responder = (dados, status = 200) =>
      new Response(JSON.stringify(dados, null, 2), {
        status,
        headers: {
          "Content-Type": "application/json; charset=utf-8",
          "Cache-Control": "no-store"
        }
      });

    try {
      // Verificacao de funcionamento da API
      if (url.pathname === "/") {
        return responder({
          projeto: "Silo Inteligente",
          status: "online",
          versao: "1.0"
        });
      }

      // Autenticacao
      if (
        !env.API_KEY ||
        request.headers.get("X-API-Key") !== env.API_KEY
      ) {
        return responder({
          erro: "Acesso nao autorizado"
        }, 401);
      }

      // Recebimento dos dados do Flespi
      if (
        request.method === "POST" &&
        url.pathname === "/insert"
      ) {
        const body = await request.json();

        const topico = body.topic || "";
        const payload = body.payload ?? body;

        const dados = typeof payload === "string"
          ? JSON.parse(payload)
          : payload;

        // Armazenar telemetria
        if (
          topico.endsWith("/telemetria") ||
          dados.temp_c !== undefined
        ) {
          if (
            !dados.dispositivo ||
            !Number.isFinite(dados.temp_c) ||
            !Number.isFinite(dados.umid_pct) ||
            dados.umid_pct < 0 ||
            dados.umid_pct > 100 ||
            !Number.isInteger(dados.ts) ||
            typeof dados.ventilador !== "boolean" ||
            typeof dados.sensor_ok !== "boolean" ||
            !["AUTO", "MANUAL"].includes(dados.modo)
          ) {
            return responder({
              erro: "Telemetria invalida"
            }, 400);
          }

          await env.DB.prepare(`
            INSERT INTO telemetria
            (dispositivo, temp_c, umid_pct,
             ventilador, sensor_ok, modo, ts)
            VALUES (?, ?, ?, ?, ?, ?, ?)
          `).bind(
            dados.dispositivo,
            dados.temp_c,
            dados.umid_pct,
            dados.ventilador ? 1 : 0,
            dados.sensor_ok ? 1 : 0,
            dados.modo,
            dados.ts
          ).run();

          return responder({
            sucesso: true,
            tabela: "telemetria"
          });
        }

        // Armazenar alertas
        if (
          topico.endsWith("/alerta") ||
          dados.alerta !== undefined
        ) {
          if (
            !dados.dispositivo ||
            typeof dados.alerta !== "string" ||
            !Number.isInteger(dados.ts)
          ) {
            return responder({
              erro: "Alerta invalido"
            }, 400);
          }

          await env.DB.prepare(`
            INSERT INTO alertas
            (dispositivo, alerta, umid_pct, ts)
            VALUES (?, ?, ?, ?)
          `).bind(
            dados.dispositivo,
            dados.alerta,
            dados.umid_pct ?? null,
            dados.ts
          ).run();

          return responder({
            sucesso: true,
            tabela: "alertas"
          });
        }

        // Armazenar confirmacoes ACK
        if (
          topico.endsWith("/ack") ||
          dados.executado !== undefined
        ) {
          if (
            !dados.id ||
            typeof dados.comando !== "string" ||
            typeof dados.executado !== "boolean" ||
            typeof dados.ventilador !== "boolean"
          ) {
            return responder({
              erro: "ACK invalido"
            }, 400);
          }

          await env.DB.prepare(`
            INSERT INTO comandos
            (comando_id, comando, executado,
             ventilador, motivo)
            VALUES (?, ?, ?, ?, ?)
          `).bind(
            dados.id,
            dados.comando,
            dados.executado ? 1 : 0,
            dados.ventilador ? 1 : 0,
            dados.motivo || ""
          ).run();

          return responder({
            sucesso: true,
            tabela: "comandos"
          });
        }

        return responder({
          sucesso: true,
          mensagem: "Mensagem sem armazenamento"
        });
      }

      // Consultar telemetria
      if (
        request.method === "GET" &&
        url.pathname === "/telemetria"
      ) {
        const resultado = await env.DB.prepare(`
          SELECT * FROM telemetria
          ORDER BY id DESC LIMIT 100
        `).all();

        return responder(resultado.results);
      }

      // Consultar alertas
      if (
        request.method === "GET" &&
        url.pathname === "/alertas"
      ) {
        const resultado = await env.DB.prepare(`
          SELECT * FROM alertas
          ORDER BY id DESC LIMIT 50
        `).all();

        return responder(resultado.results);
      }

      // Consultar comandos
      if (
        request.method === "GET" &&
        url.pathname === "/comandos"
      ) {
        const resultado = await env.DB.prepare(`
          SELECT * FROM comandos
          ORDER BY id DESC LIMIT 50
        `).all();

        return responder(resultado.results);
      }

      // Consultar ultima leitura
      if (
        request.method === "GET" &&
        url.pathname === "/ultimas"
      ) {
        const resultado = await env.DB.prepare(`
          SELECT * FROM telemetria
          ORDER BY id DESC LIMIT 1
        `).first();

        return responder(resultado || {});
      }

      return responder({
        erro: "Rota nao encontrada"
      }, 404);

    } catch (erro) {
      console.error(erro);

      return responder({
        sucesso: false,
        erro: "Falha ao processar solicitacao"
      }, 500);
    }
  }
};
