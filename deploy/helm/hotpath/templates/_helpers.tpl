{{- define "hotpath.fullname" -}}
{{- if contains .Chart.Name .Release.Name -}}{{ .Release.Name | trunc 63 | trimSuffix "-" }}{{- else -}}{{ printf "%s-%s" .Release.Name .Chart.Name | trunc 63 | trimSuffix "-" }}{{- end -}}
{{- end -}}

{{- define "hotpath.labels" -}}
app.kubernetes.io/name: {{ .Chart.Name }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version }}
{{- end -}}

{{- define "hotpath.selector" -}}
app.kubernetes.io/name: {{ .Chart.Name }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/component: engine
{{- end -}}

{{- define "hotpath.mockSelector" -}}
app.kubernetes.io/name: {{ .Chart.Name }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/component: mock-hec
{{- end -}}

{{- define "hotpath.secretName" -}}
{{- if .Values.hec.existingSecret -}}{{ .Values.hec.existingSecret }}{{- else -}}{{ include "hotpath.fullname" . }}-hec{{- end -}}
{{- end -}}

{{- define "hotpath.hecUrl" -}}
{{- if .Values.mockHec.enabled -}}
http://{{ include "hotpath.fullname" . }}-mock-hec:8088/services/collector/event
{{- else -}}
{{- required "hec.url is required when mockHec.enabled=false" .Values.hec.url -}}
{{- end -}}
{{- end -}}
