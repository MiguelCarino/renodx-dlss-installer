#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# kind-train.py -- rebuild still/bin/kind.bin, the 2D-vs-3D classifier nrstill --classify runs (kind.h).
#
#   kind-train.py prep  DATA.npz CLIPDIR...   10 frames per clip, 480x270 RGB; the label is the file
#                                             name's 2d_ / 3d_ prefix (kind-clips.txt lists ours)
#   kind-train.py export DATA.npz OUT.bin     fold MobileNetV3-Large's batch norm, fit the head
#
# Needs ffmpeg, numpy, torch, torchvision and scikit-learn; none of it ships with the app.
import os, struct, subprocess, sys
import numpy as np


def prep(out, dirs):
    W, H = 480, 270
    files = sorted(os.path.join(d, f) for d in dirs for f in os.listdir(d) if f[:3] in ("2d_", "3d_"))
    frames, labels, clip, names = [], [], [], []
    for p in files:
        du = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", p],
                            capture_output=True, text=True).stdout.strip()
        du = float(du) if du not in ("", "N/A") else 20
        got = 0
        for k in range(10):
            b = subprocess.run(["ffmpeg", "-v", "error", "-ss", "%.2f" % (du * (k + 0.5) / 10), "-i", p, "-frames:v", "1",
                                "-vf", "scale=%d:%d:flags=area,format=rgb24" % (W, H), "-f", "rawvideo", "-"],
                               capture_output=True).stdout
            if len(b) != W * H * 3:
                continue
            f = np.frombuffer(b, np.uint8).reshape(H, W, 3)
            if f.mean() < 20 or f.std() < 8:   # black or blank frames say nothing
                continue
            frames.append(f); labels.append(os.path.basename(p).startswith("2d")); clip.append(len(names)); got += 1
        if got:
            names.append(os.path.basename(p))
    np.savez_compressed(out, frames=np.array(frames), labels=np.array(labels), clip=np.array(clip), names=np.array(names))


def export(data, out):
    import torch, torchvision
    from sklearn.linear_model import LogisticRegression
    torch.set_num_threads(8)
    m=torchvision.models.mobilenet_v3_large(weights='DEFAULT').eval()
    def fold(conv, bn):
        w=conv.weight.detach().numpy().astype(np.float64); s=(bn.weight/torch.sqrt(bn.running_var+bn.eps)).detach().numpy()
        b=(bn.bias-bn.running_mean*bn.weight/torch.sqrt(bn.running_var+bn.eps)).detach().numpy()
        return (w*s[:,None,None,None]).astype(np.float32), b.astype(np.float32)
    tensors=[]; arch=[]
    f=m.features
    w,b=fold(f[0][0],f[0][1]); tensors+=[w,b]; arch.append(('stem',3,16))
    for blk in f[1:16]:
        layers=list(blk.block); d={}
        for L in layers:
            name=type(L).__name__
            if name=='Conv2dNormActivation':
                conv,bn=L[0],L[1]; w,b=fold(conv,bn); tensors+=[w,b]
                kind='dw' if conv.groups>1 else ('expand' if 'dw' not in d else 'project')
                d[kind]=(conv.in_channels,conv.out_channels,conv.kernel_size[0],conv.stride[0],type(L[2]).__name__ if len(L)>2 else 'none')
            elif name=='SqueezeExcitation':
                for c in (L.fc1,L.fc2): tensors+=[c.weight.detach().numpy().astype(np.float32)[:,:,0,0], c.bias.detach().numpy().astype(np.float32)]
                d['se']=(L.fc1.in_channels,L.fc1.out_channels)
        arch.append(d)
    w,b=fold(f[16][0],f[16][1]); tensors+=[w,b]
    # head: logistic regression on pooled 960 features of all training frames
    z=np.load(data); F=z['frames']; Y=z['labels']
    mean=torch.tensor([0.485,0.456,0.406]).view(1,3,1,1); std=torch.tensor([0.229,0.224,0.225]).view(1,3,1,1)
    fe=[]
    with torch.no_grad():
        for i in range(0,len(F),32):
            x=torch.tensor(F[i:i+32]).permute(0,3,1,2).float()/255
            x=torch.nn.functional.interpolate(x,size=(224,398),mode='area')
            fe.append(m.avgpool(m.features((x-mean)/std)).flatten(1).numpy())
    X=np.concatenate(fe); lr=LogisticRegression(C=0.1,max_iter=5000).fit(X,Y)
    tensors+=[lr.coef_[0].astype(np.float32), lr.intercept_.astype(np.float32)]
    with open(out,'wb') as o:
        o.write(b'NRKIND01'); o.write(struct.pack('<I',len(tensors)))
        for t in tensors:
            a=np.ascontiguousarray(t,dtype=np.float16).ravel(); o.write(struct.pack('<I',a.size)); o.write(a.tobytes())
    print("tensors",len(tensors),"params",sum(t.size for t in tensors), "train acc", lr.score(X,Y))


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "prep":
        prep(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) == 4 and sys.argv[1] == "export":
        export(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__ if __doc__ else "usage: kind-train.py prep|export ...")
