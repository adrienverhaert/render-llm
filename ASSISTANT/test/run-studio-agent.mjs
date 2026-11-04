import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';
const opt=(key)=>process.argv[process.argv.indexOf(key)+1];
const exe=path.resolve(opt('--exe')), evidence=path.resolve(opt('--evidence'));
const runtime=path.dirname(exe), dir=path.join(runtime,'Data/assistant');
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function until(fn,ms){const end=Date.now()+ms;while(Date.now()<end){const r=fn();if(r)return r;await delay(150);}throw new Error('Timed out');}
const read=(file)=>{try{return fs.readFileSync(file,'utf8');}catch{return '';}};
const get=(r)=>{const v=r.structuredContent;if(v?.success!==true)throw new Error(JSON.stringify(v??r));return v;};
async function connect(){const c=new Client({name:'studio-acceptance',version:'1'});await c.connect(new StdioClientTransport({command:process.execPath,args:[path.join(runtime,'ASSISTANT/dist/mcp-stdio.js'),'--dir',dir],stderr:'pipe'}));return c;}
async function stop(p){if(!p||p.exitCode!==null)return;p.kill();await until(()=>p.exitCode!==null,5000).catch(()=>{});}
fs.mkdirSync(evidence,{recursive:true});
const started=Date.now();
const editor=spawn(exe,[],{cwd:runtime,windowsHide:true,stdio:'ignore'});
let client,agent;
try{
 await delay(3500);client=await connect();
 const initial=get(await client.callTool({name:'scene_get',arguments:{}}));
 const before=JSON.parse(initial.document);before.scene.objects=[];delete before.scene.active_camera_id;before.materials={};before.animation.tracks=[];before.animation.frame_count=1;before.render.renderer='UltraFlame';
 get(await client.callTool({name:'scene_apply',arguments:{document:JSON.stringify(before),expected_revision:initial.revision}}));
 await client.close();client=undefined;
 const log=path.join(dir,'assistant.log'), offset=read(log).length;
 agent=spawn(process.execPath,[path.join(runtime,'ASSISTANT/dist/index.js'),'--dir',dir,'--model','gpt-5.6-terra','--effort','medium'],{cwd:path.join(runtime,'ASSISTANT'),windowsHide:true,stdio:['ignore','pipe','pipe']});
 let stderr='';agent.stderr.on('data',b=>{stderr+=b;});
 agent.stdout.resume();
 await until(()=>read(log).length>offset,15000);
 let seq=0;const send=r=>{fs.appendFileSync(path.join(dir,'chat_in.jsonl'),JSON.stringify({seq:++seq,t:Date.now(),...r})+'\n');fs.writeFileSync(path.join(dir,'chat_in.flag'),'');};
 const id=`studio_${Date.now()}`;
 send({type:'hello',editor_version:'studio-check',project:''});send({type:'chat',action:'new',chat_id:id});
 await until(()=>{try{return JSON.parse(read(path.join(dir,'brain.json'))).state==='idle';}catch{return false;}},15000);
 const prompt='Hola. Haz un cuarto blanco infinito. Coloca un pedestal (cilindro en medio). Coloca la cámara apuntando a él. Encima del pedestal una esfera roja. Renderiza una vista para comprobar cómo quedó.';
 const turnStart=Date.now();send({type:'user',turn_id:id,text:prompt,project_file:''});
 await until(()=>{const tail=read(log).slice(offset);if(tail.includes(`ERROR en turno ${id}:`))throw new Error(tail.slice(-1800));try{return tail.includes('[codex usage]')&&JSON.parse(read(path.join(dir,'brain.json'))).state==='idle';}catch{return false;}},240000);
 const modelMs=Date.now()-turnStart,tail=read(log).slice(offset);
 fs.writeFileSync(path.join(evidence,'agent-log.txt'),tail);
 await stop(agent);agent=undefined;client=await connect();
 const scene=get(await client.callTool({name:'scene_get',arguments:{}}));
 fs.writeFileSync(path.join(evidence,'scene.renderllm'),scene.document);
 const doc=JSON.parse(scene.document), objs=doc.scene.objects;
 const sphere=objs.find(x=>x.kind==='sphere'), pedestal=objs.find(x=>x.kind==='cylinder');
 if(!sphere||!pedestal)throw new Error('Missing sphere or pedestal');
 if(Math.abs(sphere.position[2]-(pedestal.position[2]+16*pedestal.scale[2]))>0.1)throw new Error('Sphere does not touch the pedestal top');
 if(objs.some(x=>x.kind==='plane'&&x.rotation.every(v=>v===0)))throw new Error('Vertical identity plane in studio');
 const tools=[...tail.matchAll(/tool result ([a-z_]+) ok=true:/g)].map(m=>m[1]);
 if(!tools.includes('render_image'))throw new Error('Model did not render and observe the scene');
 const render=get(await client.callTool({name:'render_image',arguments:{expected_revision:scene.revision,width:384,height:216,samples:16,denoise:true}},undefined,{timeout:180000}));
 fs.copyFileSync(path.join(dir,render.image),path.join(evidence,'studio.png'));
 const changed=JSON.parse(scene.document);changed.materials[sphere.material_id].color='#00FFFF';
 get(await client.callTool({name:'scene_validate',arguments:{document:JSON.stringify(changed)}}));
 const edited=get(await client.callTool({name:'scene_apply',arguments:{document:JSON.stringify(changed),expected_revision:scene.revision}}));
 const observed=get(await client.callTool({name:'scene_get',arguments:{}}));
 if(JSON.parse(observed.document).materials[sphere.material_id].color!=='#00FFFF')throw new Error('External edit not observed');
 get(await client.callTool({name:'undo',arguments:{expected_revision:edited.revision}}));
 const restored=get(await client.callTool({name:'scene_get',arguments:{}}));
 if(restored.document!==scene.document)throw new Error('Undo did not restore exact scene');
 const usage=[...tail.matchAll(/\[codex usage\] (\{[^\n]+\})/g)].at(-1);
 const result={status:'passed',prompt,model:'gpt-5.6-terra',effort:'medium',model_ms:modelMs,total_ms:Date.now()-started,tools,external_edit_observe_undo:true,render,usage:usage?JSON.parse(usage[1]):null};
 fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result));
}catch(error){fs.writeFileSync(path.join(evidence,'failure.txt'),String(error));throw error;}
finally{if(client)await client.close().catch(()=>{});await stop(agent);await stop(editor);}
